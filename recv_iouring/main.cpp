#include "setup.h"
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

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
    exit(-1);
  }

  struct args a;
  a.port = utils::parse_port(argv[1]);
  return a;
}

// handle_cqe_recv handles CQEvents that are stemmed from the mutlishot recv we
// have.
auto handle_cqe_recv(IOURingSetup::context &ctx, struct io_uring_cqe *cqe_head)
    -> int {
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
  // We can probably drop this checking ?
  struct io_uring_recvmsg_out *out =
      io_uring_recvmsg_validate(get_buffer(ctx, idx), cqe_head->res, &ctx.mhdr);
  if (out == nullptr) {
    std::cerr << "bad message recv, nothing recv'd" << std::endl;
    return -1;
  }

  // We should expect that namelen coincides. If this doesn't happen, then
  // we have some bad formatting.
  if (out->namelen > ctx.mhdr.msg_namelen) {
    std::cerr << "bad message recv, namelen" << std::endl;
    IOURingSetup::recycle_buffer(ctx, idx);
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

  // TODO(saiputravu):
  // Create the associated msghdr. I think there is possible optimisation we can
  // do here. For example, we can use io_uring_pre_sendmsg_zc. Also, across all
  // messages, a lot of these fields do not vary. We can set that up before
  // hand.
  //
  // NOTE: Also, we have to be careful about buffer lifetimes here as we are
  // passing over to kernel.
  ctx.send_mhdrs[idx].iov.iov_base = payload;
  ctx.send_mhdrs[idx].iov.iov_len = payload_len;
  ctx.send_mhdrs[idx].mhdr.msg_name = io_uring_recvmsg_name(out);
  ctx.send_mhdrs[idx].mhdr.msg_namelen = out->namelen;

  // io_uring_prep_sendmsg(sqe, ctx.fd, &ctx.send_mhdrs[idx].mhdr, 0);
  io_uring_prep_sendmsg(sqe, ctx.fd, &ctx.send_mhdrs[idx].mhdr, 0);

  // Set the user_data, so that the CQE lets us identify which buffer was used.
  io_uring_sqe_set_data64(sqe, idx);
  return 0;
}

auto handle_cqe_send(IOURingSetup::context &ctx, struct io_uring_cqe *cqe_head,
                     uint64_t user_data) {
  if (user_data > IOURingSetup::N_BUFFERS) {
    std::cerr << "Unknown user data found user_data=" << user_data << std::endl;
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
  return 0;
}

// handle_cqe is the on-event handler (event here being the CQEvent).
auto handle_cqe(IOURingSetup::context &ctx, struct io_uring_cqe *cqe_head)
    -> int {
  uint64_t user_data = cqe_head->user_data;
  // Here, we either got a CQE from a recv response, or send response.
  switch (user_data) {
  case IOURingSetup::RECV_ID:
    return handle_cqe_recv(ctx, cqe_head);
  default:
    // Not in the range of acceptable buffers.
    return handle_cqe_send(ctx, cqe_head, user_data);
  }
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  std::thread l([] { utils::logger(counter, alive); });

  // Setting up a global "context" so that we can just pass it around without
  // hassle. This code got slightly too convoluted to go around without structs.
  IOURingSetup::context ctx{};

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }
  ctx.fd = fd;

  // Bind to all local interfaces on the given port.
  if (utils::bind_all_interfaces(fd, a.port) < 0) {
    std::cerr << "failed to bind all interfaces" << std::endl;
    return -1;
  }

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
  auto err = io_uring_queue_init_params(IOURingSetup::N_BUFFERS, &ctx.ring, &p);
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
