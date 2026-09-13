// Runtime Evolution Fabric - runtime worker.
//
// A RuntimeWorker is one runtime generation of one component running in a real
// OS process. It holds a durable state file, negotiates its protocol and schema
// generations with the coordinator, executes migration and drain requests only
// when its negotiated operation class permits them, and refuses everything else
// without touching state.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "ref/client.hpp"
#include "ref/component.hpp"
#include "ref/ids.hpp"
#include "ref/migration.hpp"
#include "ref/protocol.hpp"
#include "ref/schema.hpp"
#include "ref/support.hpp"

namespace ref {

struct WorkerConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  RuntimeComponentId component{};
  RuntimeVersionId version{};
  RuntimeGeneration generation{};
  WorkerId worker{};
  WorkerBootId boot{};
  ProtocolId protocol{};
  ProtocolGenerationSet supported_protocols{};
  SchemaGenerationSet supported_schemas{};
  SchemaGeneration committed_schema{};
  std::vector<MigrationCapability> migrations{};
  std::vector<FeatureSupport> features{};
  CapabilityGeneration capability_generation{};
  NoteText role{};
  std::string state_path{};
  // Fault injection used by the crash-consistency proofs: the process
  // terminates after committing a migration but before acknowledging it.
  bool crash_after_migration_commit{false};
  // A real runtime reconciles on startup: it reports the state generation it
  // actually observes so an ambiguous outcome can be resolved exactly once.
  bool reconcile_on_start{true};
};

struct WorkerStats {
  std::uint64_t frames_received{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t mutations_applied{0};
  std::uint64_t mutations_rejected{0};
  std::uint64_t migrations_committed{0};
  std::uint64_t migrations_blocked{0};
  std::uint64_t drains_completed{0};
  std::uint64_t rollbacks_applied{0};
};

class RuntimeWorker {
 public:
  explicit RuntimeWorker(WorkerConfig config);
  ~RuntimeWorker();

  RuntimeWorker(const RuntimeWorker&) = delete;
  RuntimeWorker& operator=(const RuntimeWorker&) = delete;

  // Creates the durable state file in schema generation 1 when absent.
  [[nodiscard]] Status initialize_state(std::string_view owner, std::uint64_t mutation_counter);
  [[nodiscard]] Status connect_and_register(HandshakeOutcome& outcome);
  // Handles exactly one inbound coordinator frame. Returns failure when the
  // connection ends.
  [[nodiscard]] Status serve_one();
  [[nodiscard]] Status serve();
  void request_stop() noexcept { stop_requested_ = true; }

  [[nodiscard]] Status send_heartbeat();
  [[nodiscard]] Status report_reconciliation();
  // Local state mutation, gated on the negotiated operation class.
  [[nodiscard]] Status apply_mutation(std::uint64_t amount);

  [[nodiscard]] const HandshakeOutcome& handshake() const noexcept { return handshake_; }
  [[nodiscard]] OperationClass operation_class() const noexcept { return client_.operation_class(); }
  [[nodiscard]] bool drained() const noexcept { return drained_; }
  [[nodiscard]] bool stopped() const noexcept { return stop_requested_; }
  [[nodiscard]] const WorkerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const WorkerConfig& config() const noexcept { return config_; }
  [[nodiscard]] SchemaGeneration observed_schema() const;
  [[nodiscard]] std::uint64_t observed_counter() const;
  [[nodiscard]] ProtocolHandshake make_handshake() const;

 private:
  [[nodiscard]] Status handle_drain(const Frame& frame, const WireMessage& message);
  [[nodiscard]] IntegrityDigest observed_state_digest() const;
  [[nodiscard]] Status handle_migration(const Frame& frame, const WireMessage& message);
  [[nodiscard]] Status handle_rollback(const Frame& frame, const WireMessage& message);
  [[nodiscard]] Status handle_state_transform(const Frame& frame, const WireMessage& message);
  [[nodiscard]] Status send_completion(const Frame& request, const EvolutionPlanId& plan,
                                       const RolloutStageId& stage_id, StageGeneration stage_generation,
                                       MigrationOutcome outcome, IntegrityDigest digest, bool drain);
  [[nodiscard]] Status read_state(StateFile& file) const;

  WorkerConfig config_;
  EvolutionClient client_;
  HandshakeOutcome handshake_{};
  WorkerStats stats_{};
  bool drained_{false};
  bool stop_requested_{false};
  bool registered_{false};
};

}  // namespace ref
