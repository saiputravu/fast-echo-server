#include "utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>

struct args {
  ushort port;
};

uint64_t counter{0};
bool alive{true};

const uint64_t N_BUFFERS = 64 * 16;
const uint64_t BUF_SHIFT = 8; // Buf size is 256 bytes.

// We use user_data to indicate which CQE came from where. We want to know which
// buffer that we set up was used. This means that on the recv -> sending side,
// we tag with the buffer that we have to recycle. So, send -> recv'ing side
// doesn't care about this. We just set some out-of-reach ID to indicate where
// it came from.
const uint64_t RECV_ID = N_BUFFERS + 1;

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
    exit(-1);
  }

  struct args a;
  a.port = utils::parse_port(argv[1]);
  return a;
}

struct hdrs {
  struct iovec iov;
  struct msghdr mhdr;
};

struct context {
  struct io_uring ring;
  struct io_uring_buf_ring *buf_ring;

  u_char *buffer_base;
  struct msghdr mhdr;

  // Space for kernel to access concurrent locations.
  hdrs send_mhdrs[N_BUFFERS];

  long *message;

  int fd;

  size_t buf_ring_size;
};

// Chunks of 2^BUF_SHIFT.
constexpr auto buffer_size(context &ctx) -> uint64_t { return 1 << BUF_SHIFT; }
// Get indexed chunk from base.
constexpr auto get_buffer(context &ctx, uint64_t i) -> u_char * {
  return ctx.buffer_base + (i << BUF_SHIFT);
}

auto setup_buffers(context &ctx) -> int {
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

auto recycle_buffer(context &ctx, uint64_t i) {
  // Re-register buffer. When the CQE responds in this buffer, it acts as if its
  // "consumed". Therefore, we need to re-assign for the same index, the same
  // buffer.
  auto mask = io_uring_buf_ring_mask(N_BUFFERS);
  io_uring_buf_ring_add(ctx.buf_ring, get_buffer(ctx, i), buffer_size(ctx), i,
                        mask, i);
  io_uring_buf_ring_advance(ctx.buf_ring, 1);
}

// Maybe just make these into desctructers under OOP, but its too much effort
// for now.
auto cleanup_context(context &ctx) {
  munmap(ctx.buf_ring, ctx.buf_ring_size);
  // Clean up kernel side.
  io_uring_queue_exit(&ctx.ring);
}

auto start_multishot_recv(context &ctx) -> int {
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

auto handle_cqe(context &ctx, struct io_uring_cqe *cqe_head) -> int {
  uint64_t user_data = cqe_head->user_data;
  // Here, we either got a CQE from a recv response, or send response.
  switch (user_data) {
  case RECV_ID: {
    // If we receive the response to a recv request, we should perform the
    // message processing and queue a send request. We do not need to submit
    // as the main-loop is periodically submitting.

    // For more information on the flag checks below see:
    // Ref: https://github.com/axboe/liburing/discussions/1319
    // Ref:
    // https://github.com/axboe/liburing/blob/master/examples/io_uring-udp.c

    // As per man io_uring_prep_recvmsg_mutlishot, we must expect the CQE to
    // have set IORING_CQE_F_MORE flag. This flag expects more data to come,
    // which is what we should expect for multishot.
    if (!(cqe_head->flags & IORING_CQE_F_MORE)) {
      // Discard this.
      std::cerr << "IORING_CQE_F_MORE not set" << std::endl;
      return cqe_head->res;
    }

    if (cqe_head->res == -ENOBUFS) {
      // This error means that we have exhausted the kernel-side CQ buffer.
      // Just retry later (?).
      return 0;
    }

    // Other result/flag check. If IORING_CQE_F_BUFFER is not set, the event
    // was not assigned a buffer. Other than that, we do a general error res
    // check.
    if (!(cqe_head->flags & IORING_CQE_F_BUFFER) || cqe_head->res < 0) {
      std::cerr << "either IORING_CQE_F_BUFFER not set or err=" << cqe_head->res
                << std::endl;
      return cqe_head->res;
    }

    // We can finally process data and issue the send. To replicate the
    // behaviour of the other recv servers, we look into the shared buffer.
    // We use io_uring_recvmsg_validate, which will extract the message from
    // the result of a multishot.

    // The top 16 bits of this 32 bit flags is the index.
    // Ref: man io_uring_enter under IOSQE_BUFFER_SELECT.
    auto idx = cqe_head->flags >> 16;

    // Res here has the recv bytes read out.
    // We can probably drop this checking.
    struct io_uring_recvmsg_out *out = io_uring_recvmsg_validate(
        get_buffer(ctx, idx), cqe_head->res, &ctx.mhdr);
    if (out == nullptr) {
      std::cerr << "bad message recv, nothing recv'd" << std::endl;
      return -1;
    }

    // We should expect that namelen coincides. If this doesn't happen, then
    // we have some bad formatting.
    if (out->namelen > ctx.mhdr.msg_namelen) {
      std::cerr << "bad message recv, namelen" << std::endl;
      recycle_buffer(ctx, idx);
      return 0;
    }

    // Setup for sending.
    long *payload =
        reinterpret_cast<long *>(io_uring_recvmsg_payload(out, &ctx.mhdr));
    auto payload_len =
        io_uring_recvmsg_payload_length(out, cqe_head->res, &ctx.mhdr);

    auto sqe = io_uring_get_sqe(&ctx.ring);
    if (sqe == nullptr) {
      std::cerr << "unable to send" << std::endl;
      return -1;
    }

    // Processing
    for (int i = 0; i < payload_len / sizeof(uint64_t); ++i) {
      payload[i] ^= i;
    }

    ctx.send_mhdrs[idx].iov =
        iovec{.iov_base = payload, .iov_len = payload_len};
    ctx.send_mhdrs[idx].mhdr = msghdr{
        .msg_name = io_uring_recvmsg_name(out),
        .msg_namelen = out->namelen,
        .msg_iov = &ctx.send_mhdrs[idx].iov,
        .msg_iovlen = 1,
        .msg_control = nullptr,
        .msg_controllen = 0,
    };

    io_uring_prep_sendmsg(sqe, ctx.fd, &ctx.send_mhdrs[idx].mhdr, 0);
    io_uring_sqe_set_data64(sqe, idx);
  } break;
  default: {
    // Not in the range of acceptable buffers.
    if (user_data > N_BUFFERS) {
      std::cerr << "Unknown user data found user_data=" << user_data
                << std::endl;
      return -1;
    }
    // If we receive the response to a send request, we can update the counter
    // that we have tracking send requests. We only care whether the cqe was
    // successful.
    if (cqe_head->res < 0) {
      std::cerr << "send failed err=" << cqe_head->res << std::endl;
    } else {
      ++counter;
    }
    // Re-tag the buffer to be usable by kernel.
    recycle_buffer(ctx, user_data);
  }
  }
  return 0;
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  std::thread l([] { utils::logger(counter, alive); });

  // Setting up a global "context" so that we can just pass it around without
  // hassle. This code got slightly too convoluted to go around without structs.
  context ctx{};

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }
  ctx.fd = fd;

  // Bind to all local interfaces on the given port.
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr = in_addr{htonl(0)};
  addr.sin_port = a.port;
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::cerr << "failed to bind to ip=0.0.0.0 port=" << a.port
              << " errno=" << errno << std::endl;
    return -1;
  }

  ctx.message = reinterpret_cast<long *>(calloc(utils::MESSAGE_SIZE, 1));

  // We can use the nice library method provided by liburing, which saves us
  // from having to setup io_uring ourselves. This will call io_uring_setup
  // and mmap the required buffers for us.
  //
  // Ref: https://unixism.net/loti/low_level.html
  // Ref: https://man7.org/linux/man-pages/man7/io_uring_setup_flags.7.html
  struct io_uring_params p{0};
  p.sq_thread_idle = 100;
  p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER;
  // Depth matches the buffer ring so the multishot recv plus one in-flight send
  // per buffer always have an sqe to grab.
  auto err = io_uring_queue_init_params(N_BUFFERS, &ctx.ring, &p);
  if (err < 0) {
    std::cerr << "failed to setup io_uring" << a.port << " err=" << err
              << std::endl;
    return err;
  }

  // Register the provided-buffer ring the kernel writes recv'd datagrams into.
  err = setup_buffers(ctx);
  if (err < 0) {
    std::cerr << "failed to setup buffers err=" << err << std::endl;
    return err;
  }

  struct sockaddr_storage saddr;
  struct iovec iov{.iov_base = ctx.message, .iov_len = utils::MESSAGE_SIZE};
  ctx.mhdr = msghdr{.msg_name = &saddr,
                    .msg_namelen = sizeof(saddr),
                    .msg_iov = &iov,
                    .msg_iovlen = 1,
                    .msg_control = nullptr,
                    .msg_controllen = 0,
                    .msg_flags = 0};

  // io_uring_peek_batch_cqe reaps up to BATCH completions into this array.
  const unsigned BATCH = 512;
  struct io_uring_cqe *cqes[BATCH];

  err = start_multishot_recv(ctx);
  if (err < 0) {
    std::cerr << "failed to setup multishot recv err=" << err << std::endl;
    return err;
  }

  int errsz = 0;
  while (errsz != -1) {
    auto err = io_uring_submit_and_wait(&ctx.ring, 1);
    if (err < 0) {
      std::cerr << "failed to submit and wait err=" << err << std::endl;
      continue;
    }

    auto count = io_uring_peek_batch_cqe(&ctx.ring, cqes, BATCH);
    for (int i = 0; i < count; ++i) {
      err = handle_cqe(ctx, cqes[i]);
      if (err < 0) {
        std::cerr << "failed to handle idx=" << i << " err=" << err
                  << std::endl;
        continue;
      }
    }

    // Mark CQE as consumed, so that we don't reconsume the same events.
    io_uring_cq_advance(&ctx.ring, count);
  }

  if (errno) {
    std::cerr << "finished: errno=" << errno << std::endl;
  }

  alive = false;
  l.join();
  free(ctx.message);
}
