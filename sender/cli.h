#ifndef FAST_ECHO_SERVER_CLI_H
#define FAST_ECHO_SERVER_CLI_H

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/types.h>

// Parsed sender configuration.
struct Args {
  std::string ip_str;
  in_addr ip;
  ushort port;
  size_t threads = 1;
  uint64_t waitus = 10;
  bool ui = true;
};

// Parses argv. Positional <ip> <port>; options -t/--threads, -w/--wait,
// --no-ui, -h/--help. Prints usage and exit()s on --help or bad input.
Args parse_args(int argc, char *argv[]);

#endif // FAST_ECHO_SERVER_CLI_H
