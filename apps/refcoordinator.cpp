// Runtime Evolution Fabric - evolution coordinator process.
//
// Usage:
//   refcoordinator [--host 127.0.0.1] [--port 0] [--state <path>]
//
// Prints one line to stdout once it is accepting connections:
//   REF_COORDINATOR_READY port=<port> coordinator_epoch=<n> state=<path|memory>
// It then serves until stdin reaches end of file (graceful stop) or the process
// is terminated.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "cli_arguments.hpp"
#include "ref/ref.hpp"

int main(int argc, char** argv) {
  ref::cli::Arguments arguments(argc, argv);
  ref::CoordinatorConfig config;
  config.bind_host = arguments.get("host", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(arguments.number("port", 0));
  config.state_path = arguments.get("state");

  ref::EvolutionCoordinator coordinator(config);
  const ref::Status started = coordinator.start();
  if (started.is_failure()) {
    std::fprintf(stderr, "refcoordinator: start failed: %s\n", started.to_string().c_str());
    return 2;
  }
  std::printf("REF_COORDINATOR_READY port=%u coordinator_epoch=%llu state=%s\n",
              static_cast<unsigned>(coordinator.port()),
              static_cast<unsigned long long>(coordinator.coordinator_epoch().raw()),
              config.state_path.empty() ? "memory" : config.state_path.c_str());
  std::fflush(stdout);

  if (arguments.has("bootstrap-log")) {
    std::string json;
    std::printf("REF_COORDINATOR_BOOTSTRAP %s\n", json.c_str());
    std::fflush(stdout);
  }

  // Control channel: an explicit "stop"/"quit" line on stdin requests a
  // graceful shutdown. End of file on stdin does not stop the coordinator (a
  // service must not die because its controlling terminal went away); operators
  // terminate the process or send the stop line.
  std::atomic<bool> stop_requested{false};
  std::mutex stop_mutex;
  std::condition_variable stop_ready;
  std::thread control([&] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "stop" || line == "quit") {
        stop_requested = true;
        stop_ready.notify_all();
        return;
      }
      if (line.rfind("query ", 0) == 0) {
        const ref::CommandResult result =
            coordinator.command_query(coordinator.operator_identity().peer, line.substr(6));
        std::printf("REF_COORDINATOR_QUERY %s\n", result.json.c_str());
        std::fflush(stdout);
        continue;
      }
      if (line.rfind("reconcile", 0) == 0) {
        const ref::CommandResult result = coordinator.command_reconcile(coordinator.operator_identity().peer);
        std::printf("REF_COORDINATOR_RECONCILE %s\n", result.json.c_str());
        std::fflush(stdout);
        continue;
      }
      if (line.rfind("query-authority ", 0) == 0) {
        const ref::AuthorityView view = coordinator.authority(
            ref::RuntimeComponentId::from_valid(line.substr(16)));
        std::printf("REF_COORDINATOR_AUTHORITY %s\n", view.to_json().c_str());
        std::fflush(stdout);
        continue;
      }
      if (line == "stats") {
        const ref::CoordinatorStats stats = coordinator.stats();
        std::printf(
            "REF_COORDINATOR_STATS frames=%llu rejected=%llu events=%llu event_rejected=%llu queue_rejections=%llu "
            "handshakes_ok=%llu handshakes_rejected=%llu completions=%llu completions_rejected=%llu "
            "migrations=%llu rollbacks=%llu retirements=%llu fenced_boots=%llu persistence_saves=%llu\n",
            static_cast<unsigned long long>(stats.frames_received),
            static_cast<unsigned long long>(stats.frames_rejected),
            static_cast<unsigned long long>(stats.events_processed),
            static_cast<unsigned long long>(stats.events_rejected),
            static_cast<unsigned long long>(stats.queue_rejections),
            static_cast<unsigned long long>(stats.handshakes_ok),
            static_cast<unsigned long long>(stats.handshakes_rejected),
            static_cast<unsigned long long>(stats.completions_accepted),
            static_cast<unsigned long long>(stats.completions_rejected),
            static_cast<unsigned long long>(stats.migrations_committed),
            static_cast<unsigned long long>(stats.rollbacks_committed),
            static_cast<unsigned long long>(stats.retirements_committed),
            static_cast<unsigned long long>(stats.fenced_boots),
            static_cast<unsigned long long>(stats.persistence_saves));
        std::fflush(stdout);
        continue;
      }
    }
    // End of file: keep serving until the process is terminated.
  });
  {
    std::unique_lock<std::mutex> lock(stop_mutex);
    stop_ready.wait(lock, [&] { return stop_requested.load(); });
  }
  control.detach();
  coordinator.stop();
  std::printf("REF_COORDINATOR_STOPPED\n");
  std::fflush(stdout);
  return 0;
}