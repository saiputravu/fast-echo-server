#include "utils.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <utility>

struct args {
  std::string ip_str;
  in_addr ip;
  ushort port;
};

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <ip> <port>" << std::endl;
    exit(-1);
  }

  struct args a;
  a.ip_str = argv[1];
  a.ip = utils::parse_ip(argv[1]);
  a.port = utils::parse_port(argv[2]);
  return a;
}

// ON err, return INT64_MAX.
//
// Returns (id, timestamp)
auto get_ts(struct msghdr *mhdr) -> std::pair<uint64_t, uint64_t> {
  auto p = std::make_pair(UINT64_MAX, UINT64_MAX);
  for (auto *cmsg = CMSG_FIRSTHDR(mhdr); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(mhdr, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SO_TIMESTAMPING_NEW) {
      // Case 1: TX/RX Software Timestamp
      auto tsdata =
          reinterpret_cast<struct scm_timestamping64 *>(CMSG_DATA(cmsg));
      if (tsdata != nullptr) {
        p.second =
            tsdata->ts[0].tv_sec * 1'000'000'000LL + tsdata->ts[0].tv_nsec;
      }
    } else if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) {
      // Case 2: Packet ID
      auto serr = reinterpret_cast<struct sock_extended_err *>(CMSG_DATA(cmsg));
      if (serr != nullptr && serr->ee_origin == SO_EE_ORIGIN_TIMESTAMPING &&
          serr->ee_errno == ENOMSG) {
        p.first = serr->ee_data;
      }
    }
  }
  return p;
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }

  // Tries to connect to IP and port given.
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr = a.ip;
  addr.sin_port = a.port;
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "failed to connect to ip=" << a.ip_str << " port=" << a.port
              << std::endl;
    return -1;
  }

  // Setup timestamping feature on the socket. This is fine to do here as we
  // are not as latency sensitive here. We also do not want to enable hw
  // timestamping yet. Here, TX_SOFTWARE is when timestamp on transmit when
  // data leaves the kernel (generated apparently in the device driver as close
  // as possible, prior to passing packet to network interface). RX_SOFTWARE is
  // easier, this is the same as SO_TIMESTAMPNS, it is generated when data
  // enters the kernel, just after a device driver hands a packet to the kernel
  // receive stack. SOF_TIMESTAMPING_SOFTWARE allows reporting, whereas the
  // previous flags generate the timestamps. SOF_TIMESTAMPING_OPT_TSONLY makes
  // the kernel return timestamp as a cmsg alongside empty packet (i.e., doesn't
  // duplicate the packet).
  //
  // For now, this is probably a good enough proxy at taking the latency
  // measurements.
  //
  // Ref: https://docs.kernel.org/networking/timestamping.html
  int flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
              SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_OPT_ID |
              SOF_TIMESTAMPING_OPT_TSONLY;
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING_NEW, &flags, sizeof(flags)) <
      0) {
    std::cerr << "failed to set socket option flags=" << flags
              << " errno=" << errno << std::endl;
    return -1;
  }

  // Set message to all 0x3f bytes. This message gets ping-ponged back and
  // forth. We use the first sizeof(long) bytes as the timestamp.
  char *message = reinterpret_cast<char *>(malloc(utils::MESSAGE_SIZE));
  for (std::size_t i = 0; i < utils::MESSAGE_SIZE; ++i) {
    message[i] = 0x3f;
  }

  // Unbuffer the cout.
  std::cout.setf(std::ios::unitbuf);

  // Why do we use 1024? Good question.
  // Ref:
  // https://github.com/torvalds/linux/commit/8fe2f761cae9da9f9031162f104164a812ce78ab#diff-41a5260775082e3ac01bae7429e256e41a0a407ce097c99aedef4b7c00e8290dR182
  char *ctrl = reinterpret_cast<char *>(malloc(1024));

  struct msghdr mhdr{0};
  mhdr.msg_control = ctrl;
  mhdr.msg_controllen = 1024;

  // We don't set this, as we dont expect to read packet data into this at
  // all. Either on recv, we discard; on send, we don't want packet data
  // duplicated.
  //
  // Also, if we ever use this, note the side-effect comment at recvmsg
  // below. We should re-write these fields. Annoying.
  //
  // struct iovec iov{.iov_base = message, .iov_len = sizeof(message)};
  // mhdr.msg_iov = &iov;
  // mhdr.msg_iovlen = 1;

  // Busy loop send.
  while (true) {
    std::this_thread::sleep_for(std::operator""ms(2000));

    // Setup timestamp as first bytes.
    auto bef = utils::gettime();
    *reinterpret_cast<long *>(message) = bef;

    if (send(fd, message, utils::MESSAGE_SIZE, 0) < 0) {
      std::cerr << "failed send at " << bef << " errno=" << errno << std::endl;
      continue;
    }

    // recvmsg will have overwritten our mhdr, so we reset variables we care
    // about side-effects for.
    mhdr.msg_controllen = 1024;
    mhdr.msg_flags = 0;
    if (recvmsg(fd, &mhdr, 0) < 0) {
      std::cerr << "failed recv at " << bef << " errno=" << errno << std::endl;
      continue;
    }

    // Get the rx timestamp out from the ctrl headers. Archaic C macros.
    // rx_id is meaningless, recvmsg trashes our fields in iov anyways.
    auto [rx_id, rx_ts] = get_ts(&mhdr);
    if (rx_ts == UINT64_MAX) {
      std::cerr << "failed to set rx_ts" << std::endl;
      continue;
    }

    // Get the tx timestamp out the skb errqueue. Even more archaic nonsense.
    // We have to re-issue a recvmsg onto the errqueue specifically to receive
    // this. We'll reuse the old mhdr. We reset the overwritten mhdr as before.
    mhdr.msg_controllen = 1024;
    mhdr.msg_flags = 0;
    if (recvmsg(fd, &mhdr, MSG_ERRQUEUE) < 0) {
      std::cerr << "failed recv errqueue at " << bef << " errno=" << errno
                << std::endl;
      continue;
    }

    auto [tx_id, tx_ts] = get_ts(&mhdr);
    if (tx_ts == UINT64_MAX) {
      std::cerr << "failed to set tx_ts" << std::endl;
      continue;
    }
    std::cout << "send_id=" << tx_id << " send=" << tx_ts << " recv=" << rx_ts
              << " delta=" << rx_ts - tx_ts << std::endl;
  }
  free(message);
  free(ctrl);
}
