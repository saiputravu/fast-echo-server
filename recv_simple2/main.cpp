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
  struct sockaddr_in client;
  auto client_len = socklen_t{sizeof(client)};

  // First recv will be the client we always send messages to.
  int errsz =
      recvfrom(fd, message, utils::MESSAGE_SIZE, 0,
               reinterpret_cast<struct sockaddr *>(&client), &client_len);
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&client), client_len) <
      0) {
    std::cerr << "failed to connect to client ip=" << client.sin_addr.s_addr
              << " port=" << client.sin_port << " errno=" << errno << std::endl;
    return -1;
  }
  // See comment below.
  for (int i = 0; i < errsz / sizeof(uint64_t); ++i) {
    message[i] ^= i;
  }
  send(fd, message, utils::MESSAGE_SIZE, 0);

  errsz = 0;
  while (errsz != -1) {
    errsz = recv(fd, message, utils::MESSAGE_SIZE, 0);

    // Do some "processing" by setting all the bytes to 0. If the message is
    // garbled or if we receive less than the whole message, the other side
    // will know.
    for (int i = 0; i < errsz / sizeof(uint64_t); ++i) {
      message[i] ^= i;
    }

    errsz |= send(fd, message, utils::MESSAGE_SIZE, 0);
    ++counter;
  }

  if (errno) {
    std::cerr << "finished: errno=" << errno << std::endl;
  }

  alive = false;
  l.join();
  free(message);
}
