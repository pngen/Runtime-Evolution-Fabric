// Runtime Evolution Fabric - runtime worker process.
//
// Usage:
//   refworker --host 127.0.0.1 --port <port> --component <id> --generation <n>
//             --version <major.minor.patch> --worker <id> --boot <n>
//             --protocols 1,2 --schemas 1,2 --state <path> [--features a:1:pc,b:1:p]
//             [--crash-after-migration-commit] [--mutate]
//
// Two builds of this same source are used as two real runtime generations: the
// generation and its declared protocol/schema support come from the command
// line, so a generation-1 process and a generation-2 process are genuinely
// different runtime identities running as independent OS processes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "cli_arguments.hpp"
#include "ref/ref.hpp"

namespace {

ref::MigrationCapability migration_capability(const std::string& id, std::uint64_t source, std::uint64_t target) {
  ref::MigrationCapability capability;
  capability.id = ref::MigrationId::from_valid(id);
  capability.generation = ref::MigrationGeneration::from_raw(1);
  capability.schema = ref::SchemaId::from_valid("component-state");
  capability.source = ref::SchemaGeneration::from_raw(source);
  capability.target = ref::SchemaGeneration::from_raw(target);
  capability.reversible = target < source;
  capability.preserves_rollback_metadata = true;
  return capability;
}

std::vector<ref::FeatureSupport> parse_features(const std::string& csv) {
  std::vector<ref::FeatureSupport> features;
  std::size_t start = 0;
  while (start < csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string::npos ? csv.size() : comma;
    const std::string entry = csv.substr(start, end - start);
    const std::size_t first = entry.find(':');
    const std::size_t second = first == std::string::npos ? std::string::npos : entry.find(':', first + 1);
    if (first != std::string::npos) {
      ref::FeatureSupport support;
      support.feature = ref::FeatureGateId::from_valid(entry.substr(0, first));
      bool ok = false;
      const std::string generation =
          second == std::string::npos ? entry.substr(first + 1) : entry.substr(first + 1, second - first - 1);
      support.generation = ref::FeatureGateGeneration::from_raw(ref::parse_u64(generation, ok));
      const std::string flags = second == std::string::npos ? std::string() : entry.substr(second + 1);
      support.can_publish = flags.find('p') != std::string::npos;
      support.can_consume = flags.find('c') != std::string::npos;
      features.push_back(support);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return features;
}

}  // namespace

int main(int argc, char** argv) {
  ref::cli::Arguments arguments(argc, argv);
  ref::WorkerConfig config;
  config.host = arguments.get("host", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(arguments.number("port", 0));
  config.component = ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component", "component-a"));
  const auto version = ref::VersionNumber::parse(arguments.get("version", "1.0.0"));
  config.version.number = version.has_value() ? *version : ref::VersionNumber{1, 0, 0};
  config.version.artifact.build = ref::BuildId::from_valid(arguments.get("artifact", "refworker"));
  config.generation = ref::RuntimeGeneration::from_raw(arguments.number("generation", 1));
  config.worker = ref::cli::ident_arg<ref::WorkerIdTag, 63>(arguments.get("worker", "worker-1"));
  config.boot = ref::WorkerBootId::from_raw(arguments.number("boot", 1));
  config.protocol = ref::ProtocolId::from_valid("ref-wire");
  config.supported_protocols = ref::cli::protocol_set(arguments.get("protocols", "1"));
  config.supported_schemas = ref::cli::schema_set(arguments.get("schemas", "1"));
  config.committed_schema = config.supported_schemas.lowest();
  config.state_path = arguments.get("state");
  config.capability_generation = ref::CapabilityGeneration::from_raw(arguments.number("capability", 1));
  config.role = ref::NoteText::from_valid(arguments.get("role", "runtime-worker"));
  config.crash_after_migration_commit = arguments.has("crash-after-migration-commit");
  for (const auto& entry : {std::make_pair(std::string("migration-1-2"), std::make_pair(1ull, 2ull)),
                            std::make_pair(std::string("migration-2-1"), std::make_pair(2ull, 1ull)),
                            std::make_pair(std::string("migration-2-3"), std::make_pair(2ull, 3ull))}) {
    if (!config.supported_schemas.contains(ref::SchemaGeneration::from_raw(entry.second.first))) continue;
    if (!config.supported_schemas.contains(ref::SchemaGeneration::from_raw(entry.second.second))) continue;
    config.migrations.push_back(
        migration_capability(entry.first, entry.second.first, entry.second.second));
  }
  config.features = parse_features(arguments.get("features"));

  if (config.state_path.empty()) {
    std::fprintf(stderr, "refworker: --state <path> is required\n");
    return 2;
  }

  ref::RuntimeWorker worker(std::move(config));
  const ref::Status initialized = worker.initialize_state(worker.config().worker.view(), 0);
  if (initialized.is_failure()) {
    std::fprintf(stderr, "refworker: state initialization failed: %s\n", initialized.to_string().c_str());
    return 2;
  }
  ref::HandshakeOutcome outcome;
  const ref::Status connected = worker.connect_and_register(outcome);
  if (connected.is_failure()) {
    std::fprintf(stderr, "refworker: handshake failed: %s\n", connected.to_string().c_str());
    return 3;
  }
  std::printf("REF_WORKER_READY component=%s generation=%llu worker=%s boot=%llu protocol=%llu schema=%llu op=%s\n",
              worker.config().component.str().c_str(),
              static_cast<unsigned long long>(worker.config().generation.raw()),
              worker.config().worker.str().c_str(),
              static_cast<unsigned long long>(worker.config().boot.raw()),
              static_cast<unsigned long long>(outcome.negotiated_protocol.raw()),
              static_cast<unsigned long long>(outcome.negotiated_schema.raw()),
              ref::to_string(outcome.operation));
  std::fflush(stdout);

  if (arguments.has("mutate")) {
    const ref::Status mutated = worker.apply_mutation(1);
    std::printf("REF_WORKER_MUTATION result=%s applied=%llu rejected=%llu\n",
                mutated.is_ok() ? "OK" : ref::to_string(mutated.code()),
                static_cast<unsigned long long>(worker.stats().mutations_applied),
                static_cast<unsigned long long>(worker.stats().mutations_rejected));
    std::fflush(stdout);
  }

  const ref::Status served = worker.serve();
  std::printf("REF_WORKER_STOPPED reason=%s frames=%llu migrations=%llu drains=%llu mutations=%llu "
              "mutations_rejected=%llu\n",
              served.to_string().c_str(), static_cast<unsigned long long>(worker.stats().frames_received),
              static_cast<unsigned long long>(worker.stats().migrations_committed),
              static_cast<unsigned long long>(worker.stats().drains_completed),
              static_cast<unsigned long long>(worker.stats().mutations_applied),
              static_cast<unsigned long long>(worker.stats().mutations_rejected));
  std::fflush(stdout);
  return 0;
}
