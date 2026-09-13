// Runtime Evolution Fabric - benchmarks for completed operations.
//
// Every measurement is of a completed operation: setup is excluded from the
// timed region. Compatibility lookups are measured across fleet sizes so that
// accidental quadratic scans are visible in the printed scaling factor.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ref/ref.hpp"

using namespace ref;

namespace {

class Timer {
 public:
  void start() { begin_ = std::chrono::steady_clock::now(); }
  [[nodiscard]] double stop_ns(std::uint64_t operations) const {
    const auto elapsed = std::chrono::steady_clock::now() - begin_;
    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return operations == 0 ? nanoseconds : nanoseconds / static_cast<double>(operations);
  }

 private:
  std::chrono::steady_clock::time_point begin_{};
};

void report(const char* name, std::uint64_t operations, double ns_per_op) {
  std::printf("%-46s %10llu ops %12.1f ns/op\n", name,
              static_cast<unsigned long long>(operations), ns_per_op);
}

RuntimeComponentVersion make_version(const char* component, std::uint64_t generation) {
  RuntimeComponentVersion version;
  version.component = RuntimeComponentId::from_valid(component);
  version.version.number = VersionNumber{static_cast<std::uint16_t>(generation), 0, 0};
  version.version.artifact.build = BuildId::from_valid("bench");
  version.generation = RuntimeGeneration::from_raw(generation);
  (void)version.protocols.supported.add(ProtocolGeneration::from_raw(1));
  (void)version.protocols.supported.add(ProtocolGeneration::from_raw(2));
  version.protocols.readable = version.protocols.supported;
  version.protocols.writable = version.protocols.supported;
  version.protocols.minimum_safety = ProtocolGeneration::from_raw(1);
  (void)version.schemas.supported.add(SchemaGeneration::from_raw(1));
  (void)version.schemas.supported.add(SchemaGeneration::from_raw(2));
  version.schemas.readable = version.schemas.supported;
  version.schemas.writable = version.schemas.supported;
  (void)version.schemas.readable_formats.add(kStateFormatGenerationV1);
  (void)version.schemas.writable_formats.add(kStateFormatGenerationV1);
  version.capability_generation = CapabilityGeneration::from_raw(generation);
  version.provenance = EvidenceClass::Real;
  version.lifecycle = generation == 1 ? LifecycleState::Current : LifecycleState::Eligible;
  return version;
}

CompatibilityEdge make_permissive_edge(std::uint64_t from, std::uint64_t to) {
  CompatibilityEdge edge;
  edge.from = RuntimeGeneration::from_raw(from);
  edge.to = RuntimeGeneration::from_raw(to);
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    edge.aspects[i].outcome = CompatOutcome::FullyCompatible;
    edge.aspects[i].provenance = EvidenceClass::Real;
    edge.aspects[i].evidence = EvidenceGeneration::first();
  }
  edge.permissions = derive_permissions(edge);
  edge.generation = CompatibilityGeneration::first();
  edge.evidence = EvidenceGeneration::first();
  edge.epoch = EvolutionEpoch::first();
  return edge;
}

void bench_compatibility_lookup() {
  for (const std::uint64_t size : {2ull, 10ull, 100ull, 1000ull, 10000ull}) {
    CompatibilityMatrix matrix;
    CompatibilityEdge template_edge = make_permissive_edge(1, 2);
    for (std::uint64_t from = 1; from <= size; ++from) {
      for (std::uint64_t to = from + 1; to <= size && to <= from + 2; ++to) {
        template_edge.from = RuntimeGeneration::from_raw(from);
        template_edge.to = RuntimeGeneration::from_raw(to);
        (void)matrix.upsert(template_edge);
      }
    }
    Timer timer;
    const std::uint64_t iterations = 200000;
    std::uint64_t hits = 0;
    timer.start();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      const std::uint64_t from = (i % size) + 1;
      const std::uint64_t to = from + 1 <= size ? from + 1 : from;
      if (matrix.permits(RuntimeGeneration::from_raw(from), RuntimeGeneration::from_raw(to),
                         CompatPermission::ControlChannel)) {
        ++hits;
      }
    }
    std::string name = "compatibility lookup, " + std::to_string(size) + " generations, " +
                       std::to_string(matrix.edge_count()) + " edges";
    report(name.c_str(), iterations, timer.stop_ns(iterations));
    if (hits == 0) std::printf("  (no hits observed)\n");
  }
}

void bench_negotiation() {
  ProtocolRegistry registry;
  (void)registry.publish(build_wire_protocol(ProtocolGeneration::from_raw(1)));
  (void)registry.publish(build_wire_protocol(ProtocolGeneration::from_raw(2)));
  ProtocolNegotiator negotiator(&registry);
  NegotiationRequest request;
  request.protocol = ProtocolId::from_valid("ref-wire");
  (void)request.local_supported.add(ProtocolGeneration::from_raw(1));
  (void)request.local_supported.add(ProtocolGeneration::from_raw(2));
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(1));
  request.required_minimum = ProtocolGeneration::from_raw(1);
  const std::uint64_t iterations = 100000;
  Timer timer;
  timer.start();
  std::uint64_t chosen = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    chosen += negotiator.negotiate(request).chosen.raw();
  }
  report("protocol negotiation", iterations, timer.stop_ns(iterations));
  if (chosen == 0) std::printf("  (no generations negotiated)\n");
}

void bench_gates_and_rollback() {
  FeatureGateRegistry gates;
  FeatureGate gate;
  gate.id = FeatureGateId::from_valid("shadow-query");
  gate.generation = FeatureGateGeneration::first();
  gate.min_runtime_generation = RuntimeGeneration::from_raw(1);
  gate.min_protocol_generation = ProtocolGeneration::from_raw(1);
  gate.min_schema_generation = SchemaGeneration::from_raw(1);
  gate.provenance = EvidenceClass::Real;
  gate.evidence = EvidenceGeneration::first();
  (void)gates.define(gate);
  (void)gates.set_enabled(gate.id, true, EvolutionPlanId::from_valid("plan-1"), StageGeneration::first());
  GateContext context;
  context.runtime_generation = RuntimeGeneration::from_raw(2);
  context.protocol_generation = ProtocolGeneration::from_raw(2);
  context.schema_generation = SchemaGeneration::from_raw(2);
  context.peer_outcome = CompatOutcome::FullyCompatible;
  const std::uint64_t iterations = 200000;
  Timer timer;
  timer.start();
  std::uint64_t permitted = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (gates.evaluate(gate.id, context).permitted) ++permitted;
  }
  report("feature-gate resolution", iterations, timer.stop_ns(iterations));
  if (permitted == 0) std::printf("  (gate never permitted)\n");

  ComponentRegistry components;
  (void)components.publish(make_version("orders", 1));
  (void)components.publish(make_version("orders", 2));
  CompatibilityMatrix matrix;
  (void)matrix.upsert(make_permissive_edge(2, 1));
  SchemaRegistry schemas;
  (void)install_builtin_state_schemas(schemas);
  RollbackRequest request;
  request.id = RollbackId::from_valid("rollback-1");
  request.generation = RollbackGeneration::first();
  request.component = RuntimeComponentId::from_valid("orders");
  request.schema = SchemaId::from_valid("component-state");
  request.current_generation = RuntimeGeneration::from_raw(2);
  request.target_generation = RuntimeGeneration::from_raw(1);
  request.current_schema = SchemaGeneration::from_raw(1);
  request.target_schema = SchemaGeneration::from_raw(1);
  request.reverse_migration_available = true;
  request.target_process_available = true;
  request.evidence_current = true;
  request.epoch = EvolutionEpoch::first();
  request.coordinator_epoch = CoordinatorEpoch::first();
  timer.start();
  std::uint64_t allowed = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (evaluate_rollback(request, matrix, components, schemas).is_allowed()) ++allowed;
  }
  report("rollback eligibility evaluation", iterations, timer.stop_ns(iterations));
  if (allowed == 0) std::printf("  (rollback never allowed)\n");
}

void bench_persistence() {
  for (const std::uint64_t size : {2ull, 10ull, 100ull, 1000ull}) {
    DurableState state;
    state.coordinator_epoch = CoordinatorEpoch::first();
    state.evolution_epoch = EvolutionEpoch::first();
    state.evidence_generation = EvidenceGeneration::first();
    state.stage_generation = StageGeneration::first();
    state.policy_generation = PolicyGeneration::first();
    state.writer_version.number = VersionNumber{1, 0, 0};
    state.writer_version.artifact.build = BuildId::from_valid("bench");
    (void)install_builtin_state_schemas(state.schemas);
    (void)state.protocols.publish(build_wire_protocol(ProtocolGeneration::from_raw(1)));
    for (std::uint64_t generation = 1; generation <= size; ++generation) {
      (void)state.components.publish(make_version("orders", generation));
      if (generation > 1) {
        (void)state.matrix.upsert(make_permissive_edge(generation - 1, generation));
      }
      WorkerRecord worker;
      worker.worker = WorkerId::from_valid("worker-" + std::to_string(generation));
      worker.component = RuntimeComponentId::from_valid("orders");
      worker.generation = RuntimeGeneration::from_raw(generation);
      worker.last_boot = WorkerBootId::from_raw(generation);
      state.workers.emplace(worker.worker, worker);
    }
    state.matrix_generation = state.matrix.generation();
    state.gate_generation = state.gates.generation();
    const std::uint64_t iterations = size >= 1000 ? 20 : 200;
    std::string encoded;
    Timer timer;
    timer.start();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      (void)DurableStore::encode(state, encoded);
    }
    std::string name = "durable state encode, " + std::to_string(size) + " generations (" +
                       std::to_string(encoded.size() / 1024) + " KiB)";
    report(name.c_str(), iterations, timer.stop_ns(iterations));

    timer.start();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      DurableState loaded;
      (void)DurableStore::decode(encoded, loaded);
    }
    name = "durable state decode, " + std::to_string(size) + " generations";
    report(name.c_str(), iterations, timer.stop_ns(iterations));
  }
}

void bench_coordinator_operations() {
  CoordinatorConfig config;
  config.port = 0;
  EvolutionCoordinator coordinator(config);
  if (coordinator.start().is_failure()) {
    std::printf("coordinator start failed\n");
    return;
  }
  const PeerIdentity admin = coordinator.operator_identity().peer;
  (void)coordinator.command_register_component(admin, make_version("orders", 1));
  (void)coordinator.command_register_component(admin, make_version("orders", 2));
  (void)coordinator.command_publish_compatibility(admin, make_permissive_edge(1, 2));

  const std::uint64_t iterations = 20000;
  Timer timer;
  timer.start();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    (void)coordinator.authority(RuntimeComponentId::from_valid("orders"));
  }
  report("authority query (in process)", iterations, timer.stop_ns(iterations));

  PlanSpec spec;
  spec.id = EvolutionPlanId::from_valid("plan-1");
  spec.component = RuntimeComponentId::from_valid("orders");
  spec.candidate_generation = RuntimeGeneration::from_raw(2);
  spec.canary_cohort = CohortId::from_valid("canary-a");
  spec.cohorts.push_back(spec.canary_cohort);
  spec.policy.require_canary = false;
  spec.policy.require_mixed_version_cohort = false;
  spec.policy.require_new_writer_barrier = false;
  spec.policy.require_old_writer_drain = false;
  const CommandResult created = coordinator.command_create_plan(admin, spec);
  if (created.status.is_failure()) {
    std::printf("plan creation failed: %s\n", created.status.to_string().c_str());
  }
  timer.start();
  const std::uint64_t stage_iterations = 2000;
  for (std::uint64_t i = 0; i < stage_iterations; ++i) {
    (void)coordinator.command_advance_stage(admin, EvolutionPlanId::from_valid("plan-1"),
                                            RolloutStage::CompatibilityProven, StageGeneration::unset());
  }
  report("stage advance (rejected repeats included)", stage_iterations, timer.stop_ns(stage_iterations));
  coordinator.stop();
}

void bench_migration_plan() {
  SchemaRegistry registry;
  (void)install_builtin_state_schemas(registry);
  StateMigrator migrator(&registry);
  const std::string path = "bench-migration.state";
  StateObject object;
  (void)object.set_text(FieldId::from_valid("owner"), "orders");
  (void)object.set_u64(FieldId::from_valid("mutation_counter"), 1);
  (void)object.set_text(FieldId::from_valid("legacy_token"), "token-orders");
  StateFile file;
  file.header.format = kStateFormatGenerationV1;
  file.header.schema = SchemaId::from_valid("component-state");
  file.header.generation = SchemaGeneration::from_raw(1);
  file.header.epoch = EvolutionEpoch::from_raw(1);
  file.payload = object.encode();
  (void)write_state_file(path, file);
  MigrationContract contract;
  contract.id = MigrationId::from_valid("migration-1-2");
  contract.generation = MigrationGeneration::first();
  contract.schema = SchemaId::from_valid("component-state");
  contract.source = SchemaGeneration::from_raw(1);
  contract.target = SchemaGeneration::from_raw(2);
  contract.runtime_generation = RuntimeGeneration::from_raw(2);
  contract.epoch = EvolutionEpoch::from_raw(1);
  contract.plan = EvolutionPlanId::from_valid("plan-1");
  const std::uint64_t iterations = 2000;
  Timer timer;
  timer.start();
  std::uint64_t ready = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (migrator.plan_migration(path, contract).outcome == MigrationOutcome::Ready) ++ready;
  }
  report("migration plan validation (real file)", iterations, timer.stop_ns(iterations));
  if (ready == 0) std::printf("  (migration plan never became ready)\n");
  (void)remove_file_if_exists(path);
  (void)remove_file_if_exists(StateMigrator::checkpoint_path(path));
  (void)remove_file_if_exists(StateMigrator::staging_path(path));
}

void bench_worker_registration() {
  CoordinatorConfig config;
  config.port = 0;
  EvolutionCoordinator coordinator(config);
  if (coordinator.start().is_failure()) {
    std::printf("coordinator start failed\n");
    return;
  }
  const PeerIdentity admin = coordinator.operator_identity().peer;
  (void)coordinator.command_register_component(admin, make_version("orders", 1));
  const std::uint64_t iterations = 200;
  Timer timer;
  timer.start();
  std::uint64_t accepted = 0;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = coordinator.port();
    EvolutionClient client(client_config);
    ProtocolHandshake handshake;
    handshake.component = RuntimeComponentId::from_valid("orders");
    handshake.version.number = VersionNumber{1, 0, 0};
    handshake.version.artifact.build = BuildId::from_valid("bench");
    handshake.runtime_generation = RuntimeGeneration::from_raw(1);
    handshake.worker = WorkerId::from_valid("worker-" + std::to_string(i));
    handshake.boot = WorkerBootId::from_raw(1);
    handshake.protocol = ProtocolId::from_valid("ref-wire");
    (void)handshake.supported_protocols.add(ProtocolGeneration::from_raw(1));
    handshake.required_minimum = ProtocolGeneration::from_raw(1);
    (void)handshake.supported_schemas.add(SchemaGeneration::from_raw(1));
    handshake.committed_schema = SchemaGeneration::from_raw(1);
    handshake.capability_generation = CapabilityGeneration::from_raw(1);
    HandshakeOutcome outcome;
    if (client.connect(handshake, outcome).is_ok()) ++accepted;
  }
  report("worker handshake and registration (TCP)", iterations, timer.stop_ns(iterations));
  if (accepted == 0) std::printf("  (no handshakes accepted)\n");
  coordinator.stop();
}

}  // namespace

int main() {
  std::printf("Runtime Evolution Fabric %s benchmarks\n\n", REF_FABRIC_VERSION);
  bench_compatibility_lookup();
  std::printf("\n");
  bench_negotiation();
  bench_gates_and_rollback();
  std::printf("\n");
  bench_persistence();
  std::printf("\n");
  bench_coordinator_operations();
  bench_migration_plan();
  bench_worker_registration();
  return 0;
}
