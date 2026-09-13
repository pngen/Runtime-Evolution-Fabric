// Runtime Evolution Fabric - evolution coordinator.
//
// The coordinator owns the durable evolution structure and is the only
// component that may advance authority. It runs a real framed TCP service on
// one acceptor thread, one thread per connection, and exactly one event
// processing thread that owns all state mutation. No internal lock is ever held
// across socket I/O, filesystem I/O, callbacks or joins.
//
// Global lock order (highest first):
//   1. state_mutex_        durable + dynamic evolution state (leaf for reads of
//                          the registries, never held across I/O)
//   2. pending_mutex_      outbound action ledger
//   3. connections_mutex_  session table and reader thread list
//   4. Session::send_mutex leaf, never taken while holding 1-3
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ref/authority.hpp"
#include "ref/compat.hpp"
#include "ref/component.hpp"
#include "ref/ids.hpp"
#include "ref/migration.hpp"
#include "ref/plan.hpp"
#include "ref/protocol.hpp"
#include "ref/schema.hpp"
#include "ref/store.hpp"
#include "ref/support.hpp"
#include "ref/wire.hpp"

namespace ref {

struct CoordinatorConfig {
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string state_path{};  // empty == in-memory only
  std::size_t event_queue_depth{limits::kEventQueueDepth};
  std::string fabric_version{"1.0.0"};
  // Maximum age (in evidence generations) of compatibility evidence accepted
  // when validating that a plan still matches its evidence.
  std::uint64_t evidence_max_age{8};
};

struct CoordinatorStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_rejected{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t events_processed{0};
  std::uint64_t events_rejected{0};
  std::uint64_t queue_rejections{0};
  std::uint64_t persistence_saves{0};
  std::uint64_t persistence_loads{0};
  std::uint64_t handshakes_ok{0};
  std::uint64_t handshakes_rejected{0};
  std::uint64_t completions_accepted{0};
  std::uint64_t completions_rejected{0};
  std::uint64_t migrations_committed{0};
  std::uint64_t rollbacks_committed{0};
  std::uint64_t retirements_committed{0};
  std::uint64_t fenced_boots{0};
};

// Dynamic (never persisted) view of a connected runtime worker.
struct LiveWorker {
  WorkerId worker{};
  RuntimeComponentId component{};
  RuntimeGeneration generation{};
  WorkerBootId boot{};
  CapabilityGeneration capability_generation{};
  ProtocolGeneration negotiated_protocol{};
  SchemaGeneration committed_schema{};
  OperationClass operation{OperationClass::None};
  SessionId session{};
  bool active{false};
  bool draining{false};
  bool fenced{false};
  std::uint64_t frames{0};
  EvidenceGeneration evidence{};
};

// An external side effect that was registered before dispatch. Submission is
// not completion: an action stays pending until a completion is observed.
enum class ActionState : std::uint8_t { Registered = 0, Dispatched, Acknowledged, OutcomeUnknown, Abandoned };

struct PendingAction {
  std::uint64_t action_id{0};
  EvolutionPlanId plan{};
  WorkerId worker{};
  WorkerBootId boot{};
  MigrationId migration{};
  MigrationGeneration migration_generation{};
  SchemaGeneration source{};
  SchemaGeneration target{};
  ActionState state{ActionState::Registered};
  StageGeneration stage{};
  NoteText detail{};
};

struct CommandResult {
  Status status{};
  std::string json{};             // deterministic JSON document for operators
  WireMessage fields{};           // structured response fields for peers
  std::vector<std::string> explanation{};

  [[nodiscard]] bool is_ok() const noexcept { return status.is_ok(); }
};

// Specification understood by create_plan.
struct PlanSpec {
  EvolutionPlanId id{};
  RuntimeComponentId component{};
  RuntimeGeneration candidate_generation{};
  MigrationId migration{};
  MigrationGeneration migration_generation{};
  SchemaGeneration migration_source{};
  SchemaGeneration migration_target{};
  CohortId canary_cohort{};
  std::vector<CohortId> cohorts{};
  RolloutPolicy policy{};
  std::vector<RequiredEvidence> required_evidence{};
  DetailText detail{};
};

class EvolutionCoordinator {
 public:
  // Operator identity used by the CLI and by in-process administrative callers.
  // Administrative commands require it; the coordinator accepts the operator
  // flag only from loopback peers and performs no authentication of its own.
  struct OperatorIdentity {
    PeerIdentity peer;
  };

  explicit EvolutionCoordinator(CoordinatorConfig config);
  ~EvolutionCoordinator();

  EvolutionCoordinator(const EvolutionCoordinator&) = delete;
  EvolutionCoordinator& operator=(const EvolutionCoordinator&) = delete;

  // Loads durable state (when configured), starts the listener and threads.
  [[nodiscard]] Status start();
  // Idempotent, clean shutdown: stops accepting, closes sessions, joins threads.
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return listener_port_; }
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const;
  [[nodiscard]] CoordinatorStats stats() const;
  [[nodiscard]] const CoordinatorConfig& config() const noexcept { return config_; }

  // -- deterministic fabric bootstrap -------------------------------------
  // Publishes the built-in wire protocol generations and the built-in state
  // schema generations. Idempotent.
  Status install_builtin_protocols();
  Status install_builtin_schemas();
  Status install_builtin_features();

  // -- command surface (shared by the wire service and in-process callers) --
  CommandResult command_hello(const ProtocolHandshake& handshake, PeerIdentity& peer);
  CommandResult command_register_component(const PeerIdentity& peer,
                                           const RuntimeComponentVersion& version);
  CommandResult command_publish_capability(const PeerIdentity& peer, const RuntimeComponentId& component,
                                           RuntimeGeneration generation,
                                           CapabilityGeneration capability,
                                           const std::vector<FeatureSupport>& features);
  CommandResult command_publish_compatibility(const PeerIdentity& peer, const CompatibilityEdge& edge);
  CommandResult command_publish_schema(const PeerIdentity& peer, const SchemaDescriptor& descriptor);
  CommandResult command_publish_protocol(const PeerIdentity& peer, const ProtocolDescriptor& descriptor);
  CommandResult command_create_plan(const PeerIdentity& peer, const PlanSpec& spec);
  CommandResult command_advance_stage(const PeerIdentity& peer, const EvolutionPlanId& plan,
                                      RolloutStage target, StageGeneration expected_stage_generation);
  CommandResult command_rebind_plan(const PeerIdentity& peer, const EvolutionPlanId& plan,
                                    EvolutionPlanGeneration expected_generation);
  CommandResult command_enable_feature(const PeerIdentity& peer, const FeatureGateId& feature,
                                       const EvolutionPlanId& plan);
  CommandResult command_request_drain(const PeerIdentity& peer, const RuntimeComponentId& component,
                                      RuntimeGeneration generation);
  CommandResult command_request_migration(const PeerIdentity& peer, const EvolutionPlanId& plan);
  CommandResult command_request_rollback(const PeerIdentity& peer, const EvolutionPlanId& plan);
  CommandResult command_request_retirement(const PeerIdentity& peer, const RuntimeComponentId& component,
                                           RuntimeGeneration generation);
  CommandResult command_supersede_plan(const PeerIdentity& peer, const EvolutionPlanId& plan);
  CommandResult command_request_lifecycle(const PeerIdentity& peer, const RuntimeComponentId& component,
                                          RuntimeGeneration generation, LifecycleState next);
  CommandResult command_fence(const PeerIdentity& peer, const WorkerId& worker, WorkerBootId boot,
                              std::string_view reason);
  CommandResult command_publish_completion(const PeerIdentity& peer, const EvolutionPlanId& plan,
                                           const RolloutStageId& stage_id, StageGeneration stage_generation,
                                           MigrationOutcome migration_outcome,
                                           IntegrityDigest state_digest, bool drain = false);
  CommandResult command_negotiate(const PeerIdentity& peer, const ProtocolGenerationSet& supported);
  CommandResult command_query(const PeerIdentity& peer, std::string_view query,
                              const RuntimeComponentId& component = {});
  CommandResult command_reconcile(const PeerIdentity& peer);
  CommandResult command_state_transform(const PeerIdentity& peer, const EvolutionPlanId& plan,
                                        const SchemaGeneration target);

  // -- inspection ---------------------------------------------------------
  [[nodiscard]] AuthorityView authority(const RuntimeComponentId& component) const;
  [[nodiscard]] std::vector<LiveWorker> live_workers() const;
  [[nodiscard]] DurableState durable_state_copy() const;
  [[nodiscard]] OperatorIdentity operator_identity() const;
  [[nodiscard]] CoordinatorEpoch current_coordinator_epoch() const;

  // Test/diagnostic hook: forces the next persistence write to fail.
  void set_fail_persistence(bool value);

 private:
  friend class CoordinatorTestAccess;

  struct Session;
  struct Event;

  // -- internals ----------------------------------------------------------
  Status load_durable_locked();
  Status encode_locked(std::string& out) const;
  Status persist(const std::string& encoded);
  Status persist_locked_locked(std::string& encoded);

  void accept_loop();
  void session_reader(std::shared_ptr<Session> session);
  void event_loop();
  void handle_event(Event event);
  void handle_frame(const std::shared_ptr<Session>& session, const Frame& frame);
  Status validate_frame_semantics_locked(const Session& session, const Frame& frame,
                                         WireMessage& message) const;
  void respond(const std::shared_ptr<Session>& session, const Frame& request, MessageType type,
               const CommandResult& result, std::uint32_t extra_flags = 0);
  void close_session(const SessionId& id, bool notify);
  void shutdown_sessions();
  void mark_session_done(const std::shared_ptr<Session>& session, bool reader);
  void reap_sessions();

  [[nodiscard]] AuthorityView authority_locked(const RuntimeComponentId& component) const;
  [[nodiscard]] EvolutionEpoch epoch_for_locked(const RuntimeComponentId& component) const;
  [[nodiscard]] const EvolutionPlan* active_plan_locked(const RuntimeComponentId& component) const;
  [[nodiscard]] EvolutionPlan* mutable_active_plan_locked(const RuntimeComponentId& component);
  [[nodiscard]] EvolutionPlan* mutable_plan_locked(const EvolutionPlanId& id);
  [[nodiscard]] const EvolutionPlan* plan_locked(const EvolutionPlanId& id) const;
  [[nodiscard]] bool generation_has_live_workers_locked(const RuntimeComponentId& component,
                                                        RuntimeGeneration generation) const;
  [[nodiscard]] std::uint64_t live_worker_count_locked(const RuntimeComponentId& component,
                                                       RuntimeGeneration generation) const;
  [[nodiscard]] Status advance_lifecycle_locked(const RuntimeComponentId& component,
                                                RuntimeGeneration generation, LifecycleState next);
  [[nodiscard]] StagePreconditions stage_preconditions_locked(const EvolutionPlan& plan) const;
  [[nodiscard]] bool evidence_current_locked(const CompatibilityEdge& edge) const;
  void refresh_worker_authority_locked(const RuntimeComponentId& component);
  [[nodiscard]] OperationClass derive_operation_class_locked(const RuntimeComponentId& component,
                                                             RuntimeGeneration generation,
                                                             ProtocolGeneration protocol) const;
  [[nodiscard]] Status dispatch_action_locked(const PendingAction& action, WireMessage& payload);
  [[nodiscard]] Status dispatch_migration_locked(const EvolutionPlan& plan, const WorkerId& worker,
                                                 const WorkerBootId& boot);
  [[nodiscard]] Status dispatch_drain_locked(const RuntimeComponentId& component,
                                             RuntimeGeneration generation);
  [[nodiscard]] Status dispatch_rollback_state_locked(const EvolutionPlan& plan, const WorkerId& worker,
                                                      const WorkerBootId& boot);
  [[nodiscard]] Status send_to_worker_locked(const WorkerId& worker, MessageType type,
                                             const WireMessage& payload, const Frame& template_frame);
  [[nodiscard]] ReconcileReport reconcile_locked() const;
  [[nodiscard]] AuthoritySnapshot snapshot_locked(const RuntimeComponentId& component);
  [[nodiscard]] std::string query_json_locked(std::string_view query,
                                              const RuntimeComponentId& component) const;
  [[nodiscard]] Status validate_operator(const PeerIdentity& peer) const;

  CoordinatorConfig config_;
  mutable std::mutex state_mutex_{};
#ifndef NDEBUG
  mutable std::atomic<std::thread::id> state_lock_owner_{};
#else
  mutable std::atomic<std::thread::id> state_lock_owner_{};
#endif
  mutable std::mutex pending_mutex_{};
  mutable std::mutex connections_mutex_{};

  DurableState durable_{};
  std::map<WorkerId, LiveWorker> live_workers_{};
  std::vector<PendingAction> pending_actions_{};
  std::vector<AuthoritySnapshot> snapshots_{};
  std::map<SessionId, std::shared_ptr<Session>> sessions_{};
  std::map<WorkerId, SessionId> worker_sessions_{};
  CoordinatorStats stats_{};
  std::atomic<std::uint64_t> persistence_writes_{0};
  std::atomic<std::size_t> finished_sessions_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> fail_persistence_{false};
  std::uint64_t action_counter_{0};
  std::uint64_t session_counter_{0};
  std::uint64_t frame_counter_{0};

  TcpListener listener_{};
  std::uint16_t listener_port_{0};
  std::thread accept_thread_{};
  std::thread event_thread_{};
  std::vector<std::thread> reader_threads_{};
  std::unique_ptr<BoundedQueue<Event>> events_{};
};

}  // namespace ref
