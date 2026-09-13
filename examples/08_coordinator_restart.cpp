// Example 8: coordinator restart, epochs and conservative recovery.
#include <cstdio>
#include <memory>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  const std::string path = "example-08.state";
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());

  CoordinatorEpoch first_epoch{};
  {
    CoordinatorConfig config;
    config.state_path = path;
    EvolutionCoordinator coordinator(config);
    if (coordinator.start().is_failure()) return 1;
    first_epoch = coordinator.coordinator_epoch();
    std::printf("first coordinator epoch: %llu\n",
                static_cast<unsigned long long>(first_epoch.raw()));
    std::printf("authority: %s\n",
                coordinator.authority(RuntimeComponentId::from_valid("orders")).to_json().c_str());
    coordinator.stop();
  }
  {
    CoordinatorConfig config;
    config.state_path = path;
    EvolutionCoordinator coordinator(config);
    if (coordinator.start().is_failure()) return 1;
    const CoordinatorEpoch second_epoch = coordinator.coordinator_epoch();
    std::printf("after restart: coordinator epoch %llu (advanced: %s)\n",
                static_cast<unsigned long long>(second_epoch.raw()),
                second_epoch > first_epoch ? "yes" : "no");
    std::printf("live workers restored: %zu\n", coordinator.live_workers().size());
    std::printf("durable worker records kept as history: %zu\n",
                coordinator.durable_state_copy().workers.size());
    coordinator.stop();
  }
  return 0;
}
