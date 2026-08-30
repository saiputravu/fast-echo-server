#ifndef FAST_ECHO_SERVER_UTILS_H
#define FAST_ECHO_SERVER_UTILS_H

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

namespace utils {

// Size of the fixed ping-pong message. The first sizeof(long) bytes are used to
// carry a high-resolution timestamp; the remainder is padding.
inline constexpr std::size_t MESSAGE_SIZE = 256;

// Creates an IPv4 UDP socket. Returns the fd on success, or a negative value on
// failure (an error, including errno, is printed to stderr).
int make_udp_socket();

// Parses a decimal port string and returns it in network byte order (htons).
// Prints an error and exits the process on an out-of-range or malformed value.
std::uint16_t parse_port(const char *port_str);

// Parses a dotted-quad IPv4 string into an in_addr (network byte order).
// Prints an error and exits the process on a malformed value.
in_addr parse_ip(const char *ip_str);

// Returns the current high_resolution_clock time in nanoseconds since epoch.
long gettime();

} // namespace utils

#endif // FAST_ECHO_SERVER_UTILS_H
