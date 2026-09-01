#include "utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
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

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  std::thread l([] { utils::logger(counter, alive); });

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
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
  struct epoll_event events[100];

  int errsz = 0;
  while (errsz != -1) {
    auto nfds = epoll_wait(epfd, events, 100, 0);
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
