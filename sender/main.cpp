#include <arpa/inet.h>
#include <bits/chrono.h>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>

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
  inet_pton(AF_INET, a.ip_str.c_str(), &a.ip);

  a.port = htons(std::stol(argv[2]));
  return a;
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  int fd = socket(AF_INET, SOCK_DGRAM, 17);
  if (fd < 0) {
    std::cerr << "failed to open new UDP socket " << errno << std::endl;
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

  // Set message to all 0x3f bytes. This message gets ping-ponged back and
  // forth. We use the first sizeof(long) bytes as the timestamp.
  char *message = reinterpret_cast<char *>(malloc(256));
  for (int i = 0; i < 256; ++i) {
    message[i] = 0x3f;
  }

  // Unbuffer the cout.
  std::cout.setf(std::ios::unitbuf);

  // Busy loop send.
  while (true) {
    std::this_thread::sleep_for(std::operator""ms(2000));

    // Setup timestamp as first bytes.
    auto _now = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   _now.time_since_epoch())
                   .count();
    *reinterpret_cast<long *>(message) = now;

    if (send(fd, message, 256, 0) < 0) {
      std::cerr << "failed send at " << now << " errno=" << errno << std::endl;
      continue;
    }
    if (recv(fd, message, 256, 0) < 0) {
      std::cerr << "failed recv at " << now << " errno=" << errno << std::endl;
      continue;
    }

    // try receive and check the delay in response.
    auto end = *reinterpret_cast<long *>(message);
    std::cout << "start=" << now << " end=" << end << " delta=" << end - now
              << std::endl;
  }
}
