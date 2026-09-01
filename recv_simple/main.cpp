#include "utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>

struct args {
  ushort port;
  bool connected;
};

uint64_t counter{0};
bool alive{true};

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <port> [-c|--connect]" << std::endl;
    exit(-1);
  }

  struct args a;
  a.port = utils::parse_port(argv[1]);
  a.connected = argc >= 3 && (std::strcmp(argv[2], "-c") == 0 ||
                              std::strcmp(argv[2], "--connect") == 0);
  return a;
}

// Connected echo loop: learn the client from the first datagram, connect() the
// socket to it, then use the lighter recv/send calls. Returns 0 on normal exit
// or -1 if the connect() fails.
auto run_connected(int fd, long *message) -> int {
  struct sockaddr_in client;
  auto client_len = socklen_t{sizeof(client)};

  // First recv learns the client we always send messages to.
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
  return 0;
}

// Unconnected echo loop: recvfrom/sendto against whatever client addressed us,
// re-reading the source address each iteration. Returns 0 on exit.
auto run_unconnected(int fd, long *message) -> int {
  struct sockaddr_in client;
  auto client_len = socklen_t{sizeof(client)};

  int errsz = 0;
  while (errsz != -1) {
    errsz = recvfrom(fd, message, utils::MESSAGE_SIZE, 0,
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
  return 0;
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

  if (a.connected) {
    if (run_connected(fd, message) < 0) {
      return -1;
    }
  } else {
    run_unconnected(fd, message);
  }

  if (errno) {
    std::cerr << "finished: errno=" << errno << std::endl;
  }

  alive = false;
  l.join();
  free(message);
}
