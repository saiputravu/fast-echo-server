#include "utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

namespace utils {

int make_udp_socket() {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    std::cerr << "failed to open new UDP socket errno=" << errno << std::endl;
  }
  return fd;
}

std::uint16_t parse_port(const char *port_str) {
  long port = 0;
  try {
    port = std::stol(port_str);
  } catch (const std::exception &) {
    std::cerr << "invalid port: " << port_str << std::endl;
    std::exit(EXIT_FAILURE);
  }

  if (port <= 0 || port > 65535) {
    std::cerr << "port out of range (1-65535): " << port_str << std::endl;
    std::exit(EXIT_FAILURE);
  }

  return htons(static_cast<std::uint16_t>(port));
}

in_addr parse_ip(const char *ip_str) {
  in_addr ip{};
  if (inet_pton(AF_INET, ip_str, &ip) != 1) {
    std::cerr << "invalid IPv4 address: " << ip_str << std::endl;
    std::exit(EXIT_FAILURE);
  }
  return ip;
}

long gettime() {
  auto now = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             now.time_since_epoch())
      .count();
}

} // namespace utils
