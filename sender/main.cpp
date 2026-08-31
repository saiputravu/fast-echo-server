#include "sender.h"
#include "utils.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct args {
  std::string ip_str;
  in_addr ip;
  ushort port;
  size_t threads;
  uint64_t waitus;
};

auto parse_args(int argc, char *argv[]) -> args {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <ip> <port> [threads (default 1)] [timeout (us) (default 10us)]"
              << std::endl;
    exit(-1);
  }

  struct args a;
  a.ip_str = argv[1];
  a.ip = utils::parse_ip(argv[1]);
  a.port = utils::parse_port(argv[2]);

  a.threads = 1;
  a.waitus = 10;

  if (argc > 3) {
    a.threads = std::stoul(argv[3]);
  }
  if (argc > 4) {
    a.waitus = std::stoul(argv[4]);
  }
  return a;
}

std::atomic<bool> g_alive = true;

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  auto handle_signal = [](int) { g_alive.store(false); };
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);

  std::cout << "Starting " << a.threads << " threads" << std::endl;
  std::vector<std::thread> threads;
  for (int i = 0; i < a.threads; ++i) {
    threads.emplace_back([&a, i]() {
      if (runner(i, a.ip, a.port, a.ip_str, a.waitus, &g_alive) < 0) {
        std::cerr << "runner errored, finished" << std::endl;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
}
