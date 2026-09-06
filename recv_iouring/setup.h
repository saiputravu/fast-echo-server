#pragma once

#include "utils.h"
#include <bitset>
#include <cstdlib>
#include <iostream>
#include <liburing.h>
#include <mutex>
#include <sys/mman.h>

namespace IOURingSetup {
const uint64_t N_BUFFERS = 64 * 16;
const uint64_t BUF_SHIFT =
    9; // Buf size is 256 bytes, but we over estimate for headers.

// We use user_data to indicate which CQE came from where. We want to know which
// buffer that we set up was used. This means that on the recv -> sending side,
// we tag with the buffer that we have to recycle. So, send -> recv'ing side
// doesn't care about this. We just set some out-of-reach ID to indicate where
// it came from.
const uint64_t RECV_ID = N_BUFFERS + 1;

struct hdrs {
  struct iovec iov;
  struct msghdr mhdr;
};

struct context {
  context() {
    message = reinterpret_cast<long *>(calloc(utils::MESSAGE_SIZE, 1));
    if (message == nullptr) {
      std::cerr << "unable to alloc message errno=" << errno << std::endl;
    }

    for (size_t idx = 0; idx < N_BUFFERS; ++idx) {
      send_mhdrs[idx].iov = iovec{.iov_base = nullptr, .iov_len = 0};
      send_mhdrs[idx].mhdr = msghdr{
          .msg_name = nullptr,
          .msg_namelen = 0,
          .msg_iov = &send_mhdrs[idx].iov,
          .msg_iovlen = 1,
          .msg_control = nullptr,
          .msg_controllen = 0,
      };
    }
  }
  ~context() {
    if (message != nullptr) {
      free(message);
    }
  }

  struct io_uring ring;
  struct io_uring_buf_ring *buf_ring;

  u_char *buffer_base;
  struct msghdr mhdr;

  // Space for kernel to access concurrent locations.
  hdrs send_mhdrs[N_BUFFERS];
  std::bitset<N_BUFFERS> to_recycle;
  std::mutex mu;

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

// setup_buffers creates the io_uring ring buffers that are used by
// io_uring_prep_recvmsg_multishot. This mmaps some memory and partitions it
// into a bisected memory region, with the first part being the set of
// io_uring_buf_reg headers, which respectively get associated to chunks
// of space of size buffer_size(ctx).
// +------------------------------------------------------------------------+
// |   ALL io_uring_buf_reg            |   All associated dataspace         |
// +------------------------------------------------------------------------+
//
auto setup_buffers(context &ctx) -> int;

// recycle_buffer re-adds a consumed buffer (second half of the mmapped region
// in setup_buffers).
void recycle_buffer(context &ctx, uint64_t i);

auto acquire_buf(context &ctx, uint64_t i) -> bool;

auto any_bufs_left(context &ctx) -> bool;

// Maybe just make these into desctructers under OOP, but its too much effort
// for now.
auto inline cleanup_context(context &ctx) {
  munmap(ctx.buf_ring, ctx.buf_ring_size);
  // Clean up kernel side.
  io_uring_queue_exit(&ctx.ring);
}

// start_multishot_recv sets up the SQ (io_uring *) for continuous
// recvs.
auto start_multishot_recv(context &ctx) -> int;

} // namespace IOURingSetup
