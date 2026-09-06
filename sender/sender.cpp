#include "sender.h"
#include "stats.h"
#include "sync.h"
#include "utils.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

// ON err, return INT64_MAX.
//
// Returns (id, timestamp); id is only set on the tx-side
// because apparently the kernel thinks it is fine to track on the
// send side yourself :D.
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
      // Case 2: Packet ID, only on tx side, EE_DATA.
      auto serr = reinterpret_cast<struct sock_extended_err *>(CMSG_DATA(cmsg));
      if (serr != nullptr && serr->ee_origin == SO_EE_ORIGIN_TIMESTAMPING &&
          serr->ee_errno == ENOMSG) {
        p.first = serr->ee_data;
      }
    }
  }
  return p;
}

// flush_udp_errequeue will drain the tx timestamped socket control messages.
// This is expected to run in its own thread.
void flush_udp_errqueue(std::atomic<bool> *alive, int fd,
                        SyncMap<uint64_t, uint64_t> &tx_tss) {
  char *ctrl = reinterpret_cast<char *>(malloc(1024));
  struct msghdr mhdr{0};
  mhdr.msg_control = ctrl;

  while (alive->load()) {
    // See recvmsg(2) and the comment about the other location where
    // msg_controllen is used. This is overwritten with length of read control
    // message sequence.
    mhdr.msg_controllen = 1024;
    auto err = recvmsg(fd, &mhdr, MSG_ERRQUEUE);
    if (err < 0) {
      if (errno != EAGAIN) {
        std::cerr << "[flush] unable to recvmsg, errno=" << errno << std::endl;
      }
      continue;
    }

    auto [tx_id, tx_ts] = get_ts(&mhdr);
    if (tx_ts == UINT64_MAX) {
      std::cerr << "[flush] failed to set tx_ts" << std::endl;
      continue;
    }
    tx_tss.set(tx_id, tx_ts);
  }

  free(ctrl);
}

// match_tss will look through both the rx and tx timestamps and match
// based on ids. It will then clear out the matched ids. This will be used
// monitor global statistics around latency.
//
// Every 100ms it publishes a rolling-window snapshot of the latency stats for
// this thread into `metrics` (keyed by thread_id) for the dashboard to render,
// then resets the accumulator so each window reflects current conditions.
void match_tss(int thread_id, std::atomic<bool> *alive,
               SyncMap<uint64_t, uint64_t> &rx_tss,
               SyncMap<uint64_t, uint64_t> &tx_tss,
               SyncMap<int, MetricSnapshot> &metrics) {
  // Setup some space. 1000 was just chosen arbitrarily.
  std::unordered_set<uint64_t> to_delete;
  to_delete.reserve(1000);

  // Rolling-window latency statistics for this thread.
  Statistics stats;

  while (alive->load()) {
    // We iterate over tx_tss, which we care less about blocking access to.
    for (auto &[tx_id, tx_ts] : tx_tss.copy()) {
      auto res = rx_tss.find(tx_id);
      if (res) {
        // At this point, rx and tx timestamps match and are known. We can track
        // if we want. Then we queue these timestamps for removal.
        to_delete.insert(tx_id);

        auto [_, rx_ts] = res.value();
        stats.add(static_cast<double>(rx_ts - tx_ts));
      }
    }

    // Delete all matched.
    for (auto id : to_delete) {
      tx_tss.remove(id);
      rx_tss.remove(id);
    }
    to_delete.clear();

    std::this_thread::sleep_for(std::operator""ms(100));

    // Publish this window's snapshot and start the next window.
    metrics.set(thread_id, {stats.mean(), stats.stddev(), stats.count});
    stats.reset();
  }
}

int runner(int thread_id, in_addr ip, ushort port, std::string ip_str,
           uint64_t waitus, std::atomic<bool> *alive,
           SyncMap<int, MetricSnapshot> *metrics) {
  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }

  // These will map the id->timestamp so we can perform the matching later.
  SyncMap<uint64_t, uint64_t> rx_tss;
  SyncMap<uint64_t, uint64_t> tx_tss;

  // Setup background threads.
  std::thread flush{[&]() { flush_udp_errqueue(alive, fd, tx_tss); }};
  std::thread match{
      [&]() { match_tss(thread_id, alive, rx_tss, tx_tss, *metrics); }};

  // Tries to connect to IP and port given.
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr = ip;
  addr.sin_port = port;
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "failed to connect to ip=" << ip_str << " port=" << port
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

  // Add socket timeout.
  struct timeval timeout{.tv_sec = 0, .tv_usec = 200000};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    std::cerr << "failed to set socket timeout=" << timeout.tv_usec
              << " errno=" << errno << std::endl;
    return -1;
  }

  // Setup message buffers.
  uint64_t message_len = utils::MESSAGE_SIZE / sizeof(uint64_t);
  uint64_t *sendmessage =
      reinterpret_cast<uint64_t *>(calloc(message_len, sizeof(uint64_t)));
  uint64_t *recvmessage =
      reinterpret_cast<uint64_t *>(calloc(message_len, sizeof(uint64_t)));

  for (int i = 0; i < message_len; ++i) {
    sendmessage[i] = i;
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
  struct iovec iov{.iov_base = recvmessage, .iov_len = utils::MESSAGE_SIZE};
  mhdr.msg_iov = &iov;
  mhdr.msg_iovlen = 1;

  uint64_t last_id = 0;
  // Busy loop send.
  while (alive->load()) {
    std::this_thread::sleep_for(std::operator""us(waitus));

    if (send(fd, sendmessage, utils::MESSAGE_SIZE, 0) < 0) {
      std::cerr << "failed send errno=" << errno << std::endl;
      continue;
    }
    // We want to track the sent rx-id as close to success of send as possible.
    // This also means that our ID is +1 of true id. We use an unsigned value so
    // we should use last_id-1 for true id mapping.
    ++last_id;

    // As described in recvmsg(2), msg_controllen will be overwritten with the
    // length of the control message sequence read. We set it again to ensure it
    // is accurate. This also happens for msg_namelen, but we don't use it so
    // we're chilling.
    mhdr.msg_controllen = 1024;
    auto sz = recvmsg(fd, &mhdr, 0);
    if (sz < 0) {
      std::cerr << "failed recv errno=" << errno << std::endl;
      continue;
    }

    // Check message is correct. We expect sender to zero the message bytes.
    auto s = 0;
    for (int i = 0; i < message_len; ++i) {
      s += recvmessage[i];
    }
    if (s != 0) {
      std::cerr << "message was garbled, sum=" << s << " sz=" << sz
                << std::endl;
      continue;
    }

    // Get the rx timestamp out from the ctrl headers. Archaic C macros (in
    // get_ts). rx_id is meaningless, since timestamping behaviour only sets
    // this for tx side.
    auto [_, rx_ts] = get_ts(&mhdr);
    if (rx_ts == UINT64_MAX) {
      std::cerr << "failed to set rx_ts" << std::endl;
      continue;
    }

    // Track the id->timestamp mapping. This is slightly annoying, but it is
    // fine as the docs guarantee that it starts from 0 at file descriptor
    // instantiation. last_id is one ahead of id.
    rx_tss.set(last_id - 1, rx_ts);

    // The actual rx<-->tx matching is done in the background threads.
  }

  alive->store(false);
  flush.join();
  match.join();

  free(sendmessage);
  free(recvmessage);
  free(ctrl);

  return 0;
}
