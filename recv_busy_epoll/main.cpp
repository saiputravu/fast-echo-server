#include "utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>

struct args {
  ushort port;
  uint64_t busy_poll;
  bool preferred;
};

uint64_t counter{0};
bool alive{true};

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 2) {
    std::cerr
        << "Usage: " << argv[0]
        << " <port> [-b|--busy-poll (usec) (default = 100)] [-p|--preferred]"
        << std::endl;
    exit(-1);
  }

  struct args a{.busy_poll = 100, .preferred = false};
  a.port = utils::parse_port(argv[1]);

  auto compare = [](const std::string &val, const std::string &_short,
                    const std::string &_long) {
    return val == _short || val == _long;
  };

  // This is a poor way of doing this, but eh.
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    if (compare(argv[i], "-b", "--busy-poll")) {
      a.busy_poll = std::stoul(argv[++i]);
    } else if (compare(argv[i], "-p", "--prefer")) {
      a.preferred = true;
    }
  }

  return a;
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  std::thread l([] { utils::logger(counter, alive); });

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }

  if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &a.busy_poll,
                 sizeof(a.busy_poll)) < 0) {
    std::cerr << "failed to set SO_BUSY_POLL with val=" << a.busy_poll
              << " errno=" << errno << std::endl;
    return -1;
  }
  std::cout << "SO_BUSY_POLL val=" << a.busy_poll << " set" << std::endl;

  if (a.preferred) {
    auto val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &val, sizeof(val)) <
        0) {
      std::cerr << "failed to set SO_PREFER_BUSY_POL with val=" << val
                << " errno=" << errno << std::endl;
      return -1;
    }
    std::cout << "SO_PREFER_BUSY_POLL val=" << val << " set" << std::endl;
  }

  if (utils::bind_all_interfaces(fd, a.port) < 0) {
    return -1;
  }

  long *message = reinterpret_cast<long *>(calloc(utils::MESSAGE_SIZE, 1));
  struct sockaddr_in client;
  auto client_len = socklen_t{sizeof(client)};

  // Setup epolling to see read (i.e., recv) events.
  auto epfd = epoll_create1(0);
  struct epoll_event event{.events = EPOLLIN, .data = epoll_data{.fd = fd}};
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);

  // Allocate space to receive events.
  struct epoll_event events[1000];

  int errsz = 0;
  while (errsz != -1) {
    auto nfds = epoll_wait(epfd, events, 1000, 0);
    if (nfds == -1) {
      std::cerr << "failed on epoll_wait errno=" << errno << std::endl;
      break;
    }

    // Parse all the epoll events.
    for (int i = 0; i < nfds; ++i) {
      // We want to burst send the events out. As this is UDP, we don't have
      // accept to generate a new FD, which we would then have to track via
      // EPOLL.
      errsz =
          recvfrom(fd, message, utils::MESSAGE_SIZE, 0,
                   reinterpret_cast<struct sockaddr *>(&client), &client_len);

      // Do some "processing" by setting all the bytes to 0. If the message is
      // garbled or if we receive less than the whole message, the other side
      // will know.
      for (int i = 0; i < errsz / sizeof(uint64_t); ++i) {
        message[i] ^= i;
      }

      errsz |= sendto(fd, message, utils::MESSAGE_SIZE, 0,
                      reinterpret_cast<struct sockaddr *>(&client), client_len);
      ++counter;
    }
  }

  if (errno) {
    std::cerr << "finished: errno=" << errno << std::endl;
  }

  alive = false;
  l.join();
  free(message);
}
