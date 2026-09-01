#include "utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <vector>

struct args {
  ushort port;
  bool connected;
  bool preferred;
};

uint64_t counter{0};
bool alive{true};

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <port> [-c|--connect] [-p|--preferred]" << std::endl;
    exit(-1);
  }

  struct args a{.connected = false, .preferred = false};
  a.port = utils::parse_port(argv[1]);

  auto compare = [](const std::string &val, const std::string &_short,
                    const std::string &_long) {
    return val == _short || val == _long;
  };

  // This is a poor way of doing this, but eh.
  std::vector<std::string> args;
  for (int i = 2; i < argc; ++i) {
    if (compare(argv[i], "-c", "--connect")) {
      a.connected = true;
    } else if (compare(argv[i], "-p", "--prefer")) {
      a.preferred = true;
    }
  }

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

  auto val = 100;
  if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &val, sizeof(val)) < 0) {
    std::cerr << "failed to set SO_BUSY_POLL with val=" << val
              << " errno=" << errno << std::endl;
    return -1;
  }
  std::cout << "SO_BUSY_POLL val=" << val << " set" << std::endl;

  if (a.preferred) {
    val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &val, sizeof(val)) <
        0) {
      std::cerr << "failed to set SO_PREFER_BUSY_POL with val=" << val
                << " errno=" << errno << std::endl;
      return -1;
    }
    std::cout << "SO_PREFER_BUSY_POLL val=" << val << " set" << std::endl;
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
