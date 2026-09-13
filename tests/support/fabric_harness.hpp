// Runtime Evolution Fabric - shared in-process test harness for coordinator
// behaviour, property tests and deterministic race tests.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ref/ref.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

namespace reftest {

// In-process fabric with a real listener and real durable state.
class Fabric {
 public:
  explicit Fabric(const std::string& state_path) : state_path_(state_path) { start(); }
  ~Fabric() {
    if (coordinator_ != nullptr) coordinator_->stop();
  }

  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  void start() {
    ref::CoordinatorConfig config;
    config.port = 0;
    config.state_path = state_path_;
    coordinator_ = std::make_unique<ref::EvolutionCoordinator>(config);
    REF_CHECK_STATUS_OK(coordinator_->start());
    operator_ = coordinator_->operator_identity().peer;
  }

  void restart() {
    coordinator_->stop();
    coordinator_.reset();
    start();
  }

  [[nodiscard]] ref::EvolutionCoordinator& operator*() { return *coordinator_; }
  [[nodiscard]] ref::EvolutionCoordinator* operator->() { return coordinator_.get(); }
  [[nodiscard]] const ref::PeerIdentity& op() const { return operator_; }
  [[nodiscard]] std::uint16_t port() const { return coordinator_->port(); }

  [[nodiscard]] ref::CommandResult register_component(const ref::RuntimeComponentVersion& version) {
    return coordinator_->command_register_component(operator_, version);
  }
  [[nodiscard]] ref::CommandResult publish_edge(std::uint64_t from, std::uint64_t to,
                                                const char* profile = "mixed-version") {
    return coordinator_->command_publish_compatibility(operator_,
                                                       reftest::make_edge(from, to, profile));
  }
  [[nodiscard]] ref::CommandResult create_plan(const char* component, std::uint64_t candidate,
                                               const char* canary = "canary-a",
                                               const char* migration = "migration-1-2",
                                               bool require_migration_barrier = false) {
    ref::PlanSpec spec;
    spec.id = ref::EvolutionPlanId::from_valid("plan-1");
    spec.component = ref::RuntimeComponentId::from_valid(component);
    spec.candidate_generation = ref::RuntimeGeneration::from_raw(candidate);
    spec.canary_cohort = ref::CohortId::from_valid(canary);
    spec.cohorts.push_back(spec.canary_cohort);
    spec.policy.require_migration_barrier = require_migration_barrier;
    if (migration != nullptr) {
      spec.migration = ref::MigrationId::from_valid(migration);
      spec.migration_generation = ref::MigrationGeneration::from_raw(1);
      spec.migration_source = ref::SchemaGeneration::from_raw(1);
      spec.migration_target = ref::SchemaGeneration::from_raw(2);
    }
    return coordinator_->command_create_plan(operator_, spec);
  }
  [[nodiscard]] ref::CommandResult advance(ref::RolloutStage stage,
                                           ref::StageGeneration expected = ref::StageGeneration::unset()) {
    return coordinator_->command_advance_stage(operator_, ref::EvolutionPlanId::from_valid("plan-1"), stage,
                                               expected);
  }
  [[nodiscard]] ref::EvolutionPlan plan() const {
    const ref::DurableState state = coordinator_->durable_state_copy();
    const auto it = state.plans.find(ref::EvolutionPlanId::from_valid("plan-1"));
    return it == state.plans.end() ? ref::EvolutionPlan{} : it->second;
  }

 private:
  std::string state_path_;
  std::unique_ptr<ref::EvolutionCoordinator> coordinator_{};
  ref::PeerIdentity operator_{};
};

// A synthetic runtime worker session: a real socket client that has completed a
// handshake, used to drive drain, migration and completion paths.
class TestWorker {
 public:
  TestWorker(Fabric& fabric, const char* worker, std::uint64_t boot, std::uint64_t generation,
             const char* protocols, const char* schemas)
      : fabric_(&fabric) {
    ref::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = fabric_->port();
    client_ = std::make_unique<ref::EvolutionClient>(config);
    handshake_.component = ref::RuntimeComponentId::from_valid("orders");
    handshake_.version.number = ref::VersionNumber{static_cast<std::uint16_t>(generation), 0, 0};
    handshake_.version.artifact.build = ref::BuildId::from_valid("refworker");
    handshake_.runtime_generation = ref::RuntimeGeneration::from_raw(generation);
    handshake_.worker = ref::WorkerId::from_valid(worker);
    handshake_.boot = ref::WorkerBootId::from_raw(boot);
    handshake_.protocol = ref::ProtocolId::from_valid("ref-wire");
    (void)ref::decode_generation_set(protocols, handshake_.supported_protocols);
    handshake_.required_minimum = handshake_.supported_protocols.lowest();
    (void)ref::decode_generation_set(schemas, handshake_.supported_schemas);
    handshake_.committed_schema = handshake_.supported_schemas.lowest();
    handshake_.capability_generation = ref::CapabilityGeneration::from_raw(1);
    handshake_.declared_role = ref::NoteText::from_valid("test worker");
  }

  [[nodiscard]] ref::Status connect() { return client_->connect(handshake_, outcome_); }
  [[nodiscard]] const ref::HandshakeOutcome& outcome() const { return outcome_; }
  [[nodiscard]] ref::OperationClass operation() const { return client_->operation_class(); }
  [[nodiscard]] ref::EvolutionClient& client() { return *client_; }

  // Reports a completion for the plan's current stage, skipping coordinator
  // initiated frames that share the session.
  [[nodiscard]] ref::Status complete(const ref::EvolutionPlanId& plan, const ref::RolloutStageId& stage_id,
                                     ref::StageGeneration stage_generation,
                                     ref::MigrationOutcome outcome, bool drain = false) {
    ref::WireMessage payload;
    (void)payload.set_ident(ref::FieldId::from_valid("plan"), plan);
    (void)payload.set_ident(ref::FieldId::from_valid("stage_id"), stage_id);
    (void)payload.set_generation(ref::FieldId::from_valid("stage_generation"), stage_generation);
    (void)payload.set_text(ref::FieldId::from_valid("migration_outcome"), ref::to_string(outcome));
    (void)payload.set_ident(ref::FieldId::from_valid("worker"), handshake_.worker);
    (void)payload.set_generation(ref::FieldId::from_valid("boot"), handshake_.boot);
    if (drain) (void)payload.set_bool(ref::FieldId::from_valid("drain"), true);
    ref::Frame request;
    request.header.type = ref::MessageType::PublishCompletion;
    request.header.runtime_generation = handshake_.runtime_generation;
    request.header.protocol_generation = client_->negotiated_protocol();
    request.header.schema_generation = client_->negotiated_schema();
    request.header.coordinator_epoch = client_->coordinator_epoch();
    request.header.evolution_epoch = client_->evolution_epoch();
    request.header.boot = handshake_.boot;
    request.header.sequence = client_->next_sequence();
    request.payload = payload.encode();
    const ref::Status sent = ref::send_frame(client_->connection(), request);
    if (sent.is_failure()) return sent;
    for (int attempt = 0; attempt < 8; ++attempt) {
      ref::Frame response;
      const ref::Status received = ref::recv_frame(client_->connection(), response);
      if (received.is_failure()) return received;
      ref::WireMessage body;
      const ref::Status decoded = ref::WireMessage::decode(response.payload, body);
      if (decoded.is_failure()) return decoded;
      const auto status = body.text(ref::FieldId::from_valid("status"));
      if (!status.has_value() && !response.is_response()) continue;  // coordinator request
      if (!status.has_value() || *status != "OK") {
        const auto detail = body.text(ref::FieldId::from_valid("detail"));
        return ref::Status::failure(ref::ErrorCode::Conflict,
                                    detail.has_value() ? *detail : "completion refused");
      }
      return ref::Status::ok();
    }
    return ref::Status::failure(ref::ErrorCode::Conflict, "no completion response observed");
  }

 private:
  Fabric* fabric_;
  std::unique_ptr<ref::EvolutionClient> client_{};
  ref::ProtocolHandshake handshake_{};
  ref::HandshakeOutcome outcome_{};
};

// Registers generation 1 (CURRENT) and generation 2, then publishes the
// compatibility edge that makes generation 2 eligible.
inline void bootstrap_fleet(Fabric& fabric) {
  REF_CHECK_STATUS_OK(fabric.register_component(reftest::make_component(
                          "orders", 1, "1", "1", ref::LifecycleState::Current, {}, {}, "refworker-gen1"))
                          .status);
  REF_CHECK_STATUS_OK(fabric
                          .register_component(reftest::make_component(
                              "orders", 2, "1,2", "1,2", ref::LifecycleState::Registered,
                              {{1, 2}, {2, 1}, {2, 3}},
                              {reftest::feature("state-token-transform", 1, true, false),
                               reftest::feature("shadow-query", 1, true, true)},
                              "refworker-gen2"))
                          .status);
  REF_CHECK_STATUS_OK(fabric.publish_edge(1, 2).status);
  const ref::DurableState state = fabric->durable_state_copy();
  const ref::RuntimeComponentVersion* candidate = state.components.find(
      ref::RuntimeComponentId::from_valid("orders"), ref::RuntimeGeneration::from_raw(2));
  REF_CHECK(candidate != nullptr);
  if (candidate != nullptr) REF_CHECK_EQ(candidate->lifecycle, ref::LifecycleState::Eligible);
}

}  // namespace reftest
