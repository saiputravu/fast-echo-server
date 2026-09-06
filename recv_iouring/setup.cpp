#include "setup.h"
#include <atomic>
#include <bitset>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sys/mman.h>

auto IOURingSetup::setup_buffers(context &ctx) -> int {
  // Try MMAP the region of memory we want shared. We allocate N_BUFFERS worth
  // of <io_uring_buf header>+bufspace.
  ctx.buf_ring_size =
      (sizeof(struct io_uring_buf) + buffer_size(ctx)) * N_BUFFERS;
  auto mapped = mmap(nullptr, ctx.buf_ring_size, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
  if (mapped == MAP_FAILED) {
    std::cerr << "failed to mmap errno=" << errno << std::endl;
    return -1;
  }
  ctx.buf_ring = reinterpret_cast<struct io_uring_buf_ring *>(mapped);

  // TODO(saiputravu): Required setup from liburing. Should figure out why.
  io_uring_buf_ring_init(ctx.buf_ring);

  // We have successfully acquired a region of memory for all our io_uring
  // bufs. We now need to register them. See comment below. This uses the
  // first part of the carved memory region.
  auto reg = io_uring_buf_reg{
      .ring_addr = reinterpret_cast<uint64_t>(ctx.buf_ring),
      .ring_entries = N_BUFFERS,
      .bgid = 0, // Associated group id. See man page for more details.
  };

  // Compute buffer base as overhead N_BUFFER's worth of continuous headers.
  // We carve the section out into two portions (first half is just headers,
  // second half is just continuous buffers). IO_URING expects a continuous
  // set of io_uring_buf_rings, when we register them. The remaining portion
  // of the mapped memory section is for our use, associating the message
  // buffers.
  ctx.buffer_base =
      reinterpret_cast<u_char *>(reinterpret_cast<uint64_t>(ctx.buf_ring) +
                                 sizeof(struct io_uring_buf_ring) * N_BUFFERS);

  // Register the first half of the mapped memory region. See comment above.
  auto err = io_uring_register_buf_ring(&ctx.ring, &reg, 0);
  if (err < 0) {
    std::cerr << "failed to register buf ring err=" << err << std::endl;
    return err;
  }

  // Associate, for all the buffer rings registered in the code block above,
  // the buffers. I have no idea what this ring mask is, the documentation
  // doesn't really tell you, but it is needed.
  auto mask = io_uring_buf_ring_mask(N_BUFFERS);
  for (int i = 0; i < N_BUFFERS; ++i) {
    io_uring_buf_ring_add(ctx.buf_ring, get_buffer(ctx, i), buffer_size(ctx), i,
                          mask, i);
  }

  // Make buffers visible and consumable.
  io_uring_buf_ring_advance(ctx.buf_ring, N_BUFFERS);
  return 0;
}

// recycle_buffer is expected to only be called by the owner of the buffer.
// There may only be one owner in the buffer at any time.
void IOURingSetup::recycle_buffer(context &ctx, uint64_t i) {
  auto mask = io_uring_buf_ring_mask(N_BUFFERS);
  io_uring_buf_ring_add(ctx.buf_ring, get_buffer(ctx, i), buffer_size(ctx), i,
                        mask, 0);
  io_uring_buf_ring_advance(ctx.buf_ring, 1);
}

auto IOURingSetup::start_multishot_recv(context &ctx) -> int {
  // Writing an SQE:
  // 1. Get tail of SQ, check not null.
  // 2. Prep the SQE in SQ at tail.
  // 3. Submit.
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx.ring);
  if (sqe == nullptr) {
    std::cerr << "failed to get sqe" << std::endl;
    return -1;
  }

  // Important note: this method requires IOSQE_BUFFER_SELECT according to
  // its manpage. What this means is that we needed to have pre-registered
  // some buffers that the kernel side can write recvmsg data packets to.
  // MSG_TRUNC lets res report the full datagram length even when it overflows
  // the buffer. buf_group picks the group we registered in setup_buffers.
  io_uring_prep_recvmsg_multishot(sqe, ctx.fd, &ctx.mhdr, MSG_TRUNC);
  io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
  sqe->buf_group = 0;

  // For all recvs, we set user_data as RECV_ID.
  io_uring_sqe_set_data64(sqe, RECV_ID);
  return 0;
}
