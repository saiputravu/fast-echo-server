#ifndef FAST_ECHO_SERVER_SENDER_H
#define FAST_ECHO_SERVER_SENDER_H

#include "sync.h"

#include <atomic>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/types.h>
#include <utility>

struct msghdr;

// Extracts (id, timestamp) from a control-message buffer. The timestamp comes
// from the SO_TIMESTAMPING cmsg; the id (ee_data) is only present on the
// tx-side error-queue path. A field is left as UINT64_MAX when the
// corresponding cmsg is not found.
auto get_ts(struct msghdr *mhdr) -> std::pair<uint64_t, uint64_t>;

// Drains tx software timestamps off the socket error queue into tx_tss.
// Intended to run on its own thread; loops until alive becomes false.
void flush_udp_errqueue(std::atomic<bool> &alive, int fd,
                        SyncMap<uint64_t, uint64_t> &tx_tss);

// Matches rx and tx timestamps by id and accumulates latency statistics.
// Intended to run on its own thread; loops until alive becomes false.
void match_tss(std::atomic<bool> &alive, SyncMap<uint64_t, uint64_t> &rx_tss,
               SyncMap<uint64_t, uint64_t> &tx_tss);

// Connects the UDP socket fd to ip:port, enables software timestamping, and
// ping-pongs fixed-size messages while background threads collect latency
// stats. Returns a negative value on setup failure. ip_str is used only for
// logging.
int runner(int thread_id, in_addr ip, ushort port, std::string ip_str,
           uint64_t waitus, std::atomic<bool> *alive);

#endif // FAST_ECHO_SERVER_SENDER_H
