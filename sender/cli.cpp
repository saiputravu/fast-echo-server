#include "cli.h"
#include "utils.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void usage(const char *prog, int code) {
  std::cerr << "Usage: " << prog << " <ip> <port> [options]\n"
            << "\n"
            << "  -t, --threads N   number of sender threads (default 1)\n"
            << "  -w, --wait US     inter-send wait in microseconds (default 10)\n"
            << "      --no-ui       disable the TUI; print periodic summary stats\n"
            << "  -h, --help        show this help\n";
  std::exit(code);
}

} // namespace

Args parse_args(int argc, char *argv[]) {
  Args a;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    // Consumes and returns the value following a flag, or errors out.
    auto value = [&](const char *flag) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << flag << " requires a value" << std::endl;
        usage(argv[0], -1);
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      usage(argv[0], 0);
    } else if (arg == "--no-ui") {
      a.ui = false;
    } else if (arg == "-t" || arg == "--threads") {
      a.threads = std::stoul(value("--threads"));
    } else if (arg == "-w" || arg == "--wait") {
      a.waitus = std::stoul(value("--wait"));
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "unknown option: " << arg << std::endl;
      usage(argv[0], -1);
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() < 2) {
    std::cerr << "expected <ip> and <port>" << std::endl;
    usage(argv[0], -1);
  }

  a.ip_str = positional[0];
  a.ip = utils::parse_ip(positional[0].c_str());
  a.port = utils::parse_port(positional[1].c_str());
  return a;
}
