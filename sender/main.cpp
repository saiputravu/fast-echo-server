#include "cli.h"
#include "sender.h"
#include "stats.h"
#include "sync.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

std::atomic<bool> g_alive = true;

// Builds an FTXUI GraphFunction from a copy of a time-series. It right-aligns
// the most recent `width` points and scales them to the drawing height.
static auto make_series(std::vector<double> data) {
  return [data](int width, int height) {
    std::vector<int> out(width, 0);
    if (data.empty() || height <= 0) {
      return out;
    }
    double maxv = 0;
    for (double v : data) {
      maxv = std::max(maxv, v);
    }
    int n = static_cast<int>(data.size());
    for (int x = 0; x < width; ++x) {
      int idx = n - width + x; // align the newest sample to the right edge.
      if (idx < 0) {
        continue;
      }
      out[x] = maxv > 0 ? static_cast<int>(data[idx] / maxv * (height - 1)) : 0;
    }
    return out;
  };
}

auto main(int argc, char *argv[]) -> int {
  auto a = parse_args(argc, argv);

  auto handle_signal = [](int) { g_alive.store(false); };
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);

  // Each worker publishes its latest 100ms stats snapshot here, keyed by
  // thread id. The consumer thread turns these into time-series for the graphs.
  SyncMap<int, MetricSnapshot> metrics;

  // Spawn the workers.
  std::vector<std::thread> threads;
  for (int i = 0; i < a.threads; ++i) {
    threads.emplace_back([&a, i, &metrics]() {
      if (runner(i, a.ip, a.port, a.ip_str, a.waitus, &g_alive, &metrics) < 0) {
        std::cerr << "runner errored, finished" << std::endl;
      }
    });
  }

  // Headless mode: no TUI. Print a tidy per-thread + aggregate line every
  // second, and a final summary (over the sampled aggregate means) on exit.
  if (!a.ui) {
    Statistics agg;
    auto sample = [&](const char *tag) {
      auto snap = metrics.copy();
      double sum = 0;
      std::cout << tag;
      for (int i = 0; i < a.threads; ++i) {
        auto it = snap.find(i);
        MetricSnapshot s = it != snap.end() ? it->second : MetricSnapshot{};
        std::cout << "  t" << i << " mean=" << s.mean << " sd=" << s.stddev
                  << " n=" << s.count;
        sum += s.mean;
      }
      double a_mean = a.threads ? sum / a.threads : 0.0;
      std::cout << "  | agg=" << a_mean << std::endl;
      return a_mean;
    };

    while (g_alive.load()) {
      std::this_thread::sleep_for(std::operator""ms(1000));
      agg.add(sample("[stats]"));
    }

    for (auto &t : threads) {
      t.join();
    }
    std::cout << "[final] agg mean=" << agg.mean() << " sd=" << agg.stddev()
              << " samples=" << agg.count << std::endl;
    return 0;
  }

  // Dashboard model: per-thread mean history + an aggregate (mean-of-means)
  // history, plus the latest snapshot per thread for labels. Guarded by mu.
  constexpr size_t CAP = 3000;
  std::mutex mu;
  std::vector<std::deque<double>> history(a.threads);
  std::deque<double> aggregate;
  std::vector<MetricSnapshot> latest(a.threads);

  auto screen = ftxui::ScreenInteractive::Fullscreen();

  // Consumer: sample the shared map every 100ms, extend the time-series, and
  // ask FTXUI to redraw.
  std::thread consumer([&]() {
    while (g_alive.load()) {
      std::this_thread::sleep_for(std::operator""ms(100));
      auto snap = metrics.copy();
      {
        std::scoped_lock lk(mu);
        double sum = 0;
        for (int i = 0; i < a.threads; ++i) {
          auto it = snap.find(i);
          MetricSnapshot s = it != snap.end() ? it->second : MetricSnapshot{};
          latest[i] = s;

          // Max cap the e
          history[i].push_back(s.mean);
          if (history[i].size() > CAP) {
            history[i].pop_front();
          }
          sum += s.mean;
        }
        aggregate.push_back(a.threads ? sum / a.threads : 0.0);
        if (aggregate.size() > CAP) {
          aggregate.pop_front();
        }
      }
      screen.PostEvent(ftxui::Event::Custom);
    }
  });

  using namespace ftxui;
  auto renderer = Renderer([&]() {
    // Snapshot the model under the lock, then build the view lock-free.
    std::vector<std::vector<double>> hist_copy(a.threads);
    std::vector<double> agg_copy;
    std::vector<MetricSnapshot> latest_copy;
    {
      std::scoped_lock lk(mu);
      for (int i = 0; i < a.threads; ++i) {
        hist_copy[i].assign(history[i].begin(), history[i].end());
      }
      agg_copy.assign(aggregate.begin(), aggregate.end());
      latest_copy = latest;
    }

    Elements rows;
    rows.push_back(text("sender dashboard  threads=" + std::to_string(a.threads) +
                        "  window=100ms  q:quit") |
                   bold);
    rows.push_back(window(text("aggregate mean latency (ns)"),
                          graph(make_series(agg_copy)) | color(Color::Green) |
                              flex) |
                   flex);
    for (int i = 0; i < a.threads; ++i) {
      std::string label = "thread " + std::to_string(i) +
                          "  mean=" + std::to_string(latest_copy[i].mean) +
                          "  sd=" + std::to_string(latest_copy[i].stddev) +
                          "  n=" + std::to_string(latest_copy[i].count);
      rows.push_back(window(text(label),
                            graph(make_series(hist_copy[i])) |
                                color(Color::Cyan) | flex) |
                     flex);
    }
    return vbox(std::move(rows)) | flex;
  });

  // Quit on 'q' or Escape.
  renderer |= CatchEvent([&](Event e) {
    if (e == Event::Character('q') || e == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(renderer);

  // Loop returned (q / Escape / Ctrl-C): tear everything down.
  g_alive.store(false);

  consumer.join();
  for (auto &t : threads) {
    t.join();
  }
  return 0;
}
