#include "utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>

struct args {
  ushort port;
};

uint64_t counter{0};
uint64_t last_seen_counter{0};
bool alive{true};

auto logger() {
  while (alive) {
    uint64_t counter_now = counter;
    if (last_seen_counter < counter_now) {
      // >= 1 packet sent.
      auto now = std::chrono::high_resolution_clock::now();
      std::cerr << now.time_since_epoch().count() << ": sent " << counter_now - last_seen_counter
                << " msg(s) recently" << std::endl;
      last_seen_counter = std::max(counter_now, last_seen_counter);
    }
    std::this_thread::sleep_for(std::operator""ms(200));
  }
}

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

  std::thread l(logger);

  int fd = utils::make_udp_socket();
  if (fd < 0) {
    return fd;
  }

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

  long *message = reinterpret_cast<long *>(calloc(utils::MESSAGE_SIZE, 1));
  int err;
  struct sockaddr_in client;
  auto client_len = socklen_t{sizeof(client)};

  while (err != 0) {
    err = recvfrom(fd, message, utils::MESSAGE_SIZE, 0,
                   reinterpret_cast<struct sockaddr *>(&client), &client_len);
    message[1] = utils::gettime();
    err |= sendto(fd, message, utils::MESSAGE_SIZE, 0,
                  reinterpret_cast<struct sockaddr *>(&client), client_len);
    ++counter;
  }

  std::cerr << "finished: errno=" << errno << std::endl;
  alive = false;
  l.join();
  free(message);
}
