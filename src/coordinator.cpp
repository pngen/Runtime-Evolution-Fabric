#include "coordinator_internal.hpp"
#include "ref/coordinator.hpp"

#include <algorithm>
#include <array>

namespace ref {
CoordinatorEpoch EvolutionCoordinator::coordinator_epoch() const { return current_coordinator_epoch(); }

CoordinatorEpoch EvolutionCoordinator::current_coordinator_epoch() const {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  return durable_.coordinator_epoch;
}

CoordinatorStats EvolutionCoordinator::stats() const {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  CoordinatorStats snapshot = stats_;
  snapshot.persistence_saves = persistence_writes_.load();
  return snapshot;
}

void EvolutionCoordinator::set_fail_persistence(bool value) { fail_persistence_ = value; }

EvolutionCoordinator::OperatorIdentity EvolutionCoordinator::operator_identity() const {
  OperatorIdentity identity;
  identity.peer.session = SessionId::from_valid("operator");
  identity.peer.is_operator = true;
  identity.peer.authenticated = true;
  identity.peer.coordinator_epoch = current_coordinator_epoch();
  return identity;
}

Status EvolutionCoordinator::validate_operator(const PeerIdentity& peer) const {
  if (!peer.is_operator) {
    return Status::failure(ErrorCode::Unauthorized, "command requires operator authority");
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Built-in protocol and schema bootstrap
// ---------------------------------------------------------------------------
Status EvolutionCoordinator::install_builtin_protocols() {
  std::string encoded;
  Status status;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    for (const auto raw : {1ull, 2ull}) {
      const Status published = durable_.protocols.publish(build_wire_protocol(ProtocolGeneration::from_raw(raw)));
      if (published.is_failure()) return published;
    }
    status = encode_locked(encoded);
  }
  if (status.is_failure()) return status;
  return persist(encoded);
}

Status EvolutionCoordinator::install_builtin_schemas() {
  std::string encoded;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status installed = install_builtin_state_schemas(durable_.schemas);
    if (installed.is_failure()) return installed;
    const Status encoded_status = encode_locked(encoded);
    if (encoded_status.is_failure()) return encoded_status;
  }
  return persist(encoded);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
EvolutionEpoch EvolutionCoordinator::epoch_for_locked(const RuntimeComponentId& component) const {
  const EvolutionPlan* plan = active_plan_locked(component);
  if (plan != nullptr) return plan->epoch;
  return EvolutionEpoch::unset();
}

const EvolutionPlan* EvolutionCoordinator::active_plan_locked(const RuntimeComponentId& component) const {
  const EvolutionPlan* best = nullptr;
  for (const auto& [id, plan] : durable_.plans) {
    (void)id;
    if (plan.component != component) continue;
    if (!plan.is_active()) continue;
    if (best == nullptr || plan.epoch > best->epoch) best = &plan;
  }
  return best;
}

EvolutionPlan* EvolutionCoordinator::mutable_active_plan_locked(const RuntimeComponentId& component) {
  EvolutionPlan* best = nullptr;
  for (auto& [id, plan] : durable_.plans) {
    (void)id;
    if (plan.component != component) continue;
    if (!plan.is_active()) continue;
    if (best == nullptr || plan.epoch > best->epoch) best = &plan;
  }
  return best;
}

EvolutionPlan* EvolutionCoordinator::mutable_plan_locked(const EvolutionPlanId& id) {
  const auto it = durable_.plans.find(id);
  return it == durable_.plans.end() ? nullptr : &it->second;
}

const EvolutionPlan* EvolutionCoordinator::plan_locked(const EvolutionPlanId& id) const {
  const auto it = durable_.plans.find(id);
  return it == durable_.plans.end() ? nullptr : &it->second;
}

std::uint64_t EvolutionCoordinator::live_worker_count_locked(const RuntimeComponentId& component,
                                                             RuntimeGeneration generation) const {
  std::uint64_t count = 0;
  for (const auto& [id, worker] : live_workers_) {
    (void)id;
    if (worker.component == component && worker.generation == generation && worker.active &&
        !worker.fenced) {
      ++count;
    }
  }
  return count;
}

bool EvolutionCoordinator::generation_has_live_workers_locked(const RuntimeComponentId& component,
                                                              RuntimeGeneration generation) const {
  return live_worker_count_locked(component, generation) > 0;
}

Status EvolutionCoordinator::advance_lifecycle_locked(const RuntimeComponentId& component,
                                                      RuntimeGeneration generation, LifecycleState next) {
  return durable_.components.set_lifecycle(component, generation, next);
}

bool EvolutionCoordinator::evidence_current_locked(const CompatibilityEdge& edge) const {
  const RuntimeGeneration authoritative = edge.from;
  (void)authoritative;
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    if (!edge.aspects[i].is_current(durable_.evidence_generation, config_.evidence_max_age)) {
      if (edge.aspects[i].outcome == CompatOutcome::Unknown) return false;
    }
  }
  return true;
}

OperationClass EvolutionCoordinator::derive_operation_class_locked(const RuntimeComponentId& component,
                                                                   RuntimeGeneration generation,
                                                                   ProtocolGeneration protocol) const {
  const RuntimeComponentVersion* version = durable_.components.find(component, generation);
  if (version == nullptr) return OperationClass::None;
  if (is_terminal_lifecycle(version->lifecycle)) return OperationClass::None;
  const EvolutionPlan* plan = active_plan_locked(component);
  if (plan == nullptr) {
    // No evolution in progress: only the authoritative generation may mutate.
    if (is_authoritative_lifecycle(version->lifecycle)) {
      return permits_mutation_lifecycle(version->lifecycle) ? OperationClass::FullMutation
                                                            : OperationClass::ReadOnly;
    }
    return OperationClass::ReadOnly;
  }
  // A peer that negotiated below the protocol generation the plan currently
  // requires can never be granted full mutation authority.
  const bool protocol_below_plan = protocol.is_set() && protocol < plan->protocol_generation;

  if (generation == plan->candidate_generation) {
    OperationClass operation = OperationClass::None;
    switch (plan->stage) {
      case RolloutStage::CandidateRegistered:
      case RolloutStage::CompatibilityProven:
        operation = OperationClass::None;
        break;
      case RolloutStage::CanaryCohort:
        operation = OperationClass::ReadOnly;  // canary: shadow and query only
        break;
      case RolloutStage::MixedVersionCohort:
      case RolloutStage::ExpandedCohort:
      case RolloutStage::MigrationBarrier:
        operation = OperationClass::RestrictedMutation;
        break;
      case RolloutStage::NewWriterEnabled:
      case RolloutStage::OldWriterDrain:
      case RolloutStage::FullPromotion:
      case RolloutStage::OldGenerationRetirement:
        operation = OperationClass::FullMutation;
        break;
      default:
        operation = OperationClass::ReadOnly;
        break;
    }
    if (protocol_below_plan && operation == OperationClass::FullMutation) {
      operation = OperationClass::RestrictedMutation;
    }
    return operation;
  }
  if (generation == plan->current_generation) {
    if (plan->stage >= RolloutStage::OldWriterDrain) return OperationClass::ReadOnly;
    return OperationClass::FullMutation;
  }
  return OperationClass::ReadOnly;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
Status EvolutionCoordinator::encode_locked(std::string& out) const {
  return DurableStore::encode(durable_, out);
}

Status EvolutionCoordinator::persist_locked_locked(std::string& encoded) { return persist(encoded); }

Status EvolutionCoordinator::persist(const std::string& encoded) {
  if (config_.state_path.empty()) return Status::ok();
  if (fail_persistence_.load()) {
    return Status::failure(ErrorCode::IoFailure, "persistence failure injected");
  }
  const Status written = write_file_atomic(config_.state_path, encoded);
  if (written.is_ok()) persistence_writes_.fetch_add(1);
  return written;
}

Status EvolutionCoordinator::load_durable_locked() {
  // Baseline generations for a coordinator that has never persisted anything.
  durable_.coordinator_epoch = CoordinatorEpoch::first();
  durable_.evolution_epoch = EvolutionEpoch::first();
  durable_.evidence_generation = EvidenceGeneration::first();
  durable_.stage_generation = StageGeneration::first();
  durable_.snapshot_generation = SnapshotGeneration::first();
  durable_.policy_generation = PolicyGeneration::first();
  // The compatibility matrix and the feature-gate table start empty, so their
  // generations stay unset until the first record is published.
  durable_.matrix_generation = CompatibilityGeneration::unset();
  durable_.gate_generation = FeatureGateGeneration::unset();
  durable_.replay_watermark = SnapshotGeneration::unset();
  if (config_.state_path.empty()) return Status::ok();
  DurableStore store(config_.state_path);
  if (!store.exists()) return Status::ok();
  DurableState loaded;
  const Status status = store.load(loaded);
  if (status.is_failure()) return status;
  durable_ = std::move(loaded);
  // Recovery: advance the coordinator epoch, drop every dynamic liveness fact
  // and require workers to re-register and renegotiate.
  durable_.coordinator_epoch = durable_.coordinator_epoch.is_set() ? durable_.coordinator_epoch.next()
                                                                   : CoordinatorEpoch::first();
  live_workers_.clear();
  worker_sessions_.clear();
  for (auto& [id, record] : durable_.workers) {
    (void)id;
    record.drained = false;
    record.detail = DetailText::from_valid("awaiting re-registration after coordinator restart");
  }
  ++stats_.persistence_loads;
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Authority view
// ---------------------------------------------------------------------------
AuthorityView EvolutionCoordinator::authority(const RuntimeComponentId& component) const {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  return authority_locked(component);
}

AuthorityView EvolutionCoordinator::authority_locked(const RuntimeComponentId& component) const {
  AuthorityView view;
  view.component = component;
  view.epoch = durable_.evolution_epoch;
  view.coordinator_epoch = durable_.coordinator_epoch;
  view.snapshot_generation = durable_.snapshot_generation;

  const auto authoritative = durable_.components.authoritative_generation(component);
  if (authoritative.has_value()) {
    view.has_authoritative = true;
    view.authoritative = *authoritative;
  }

  const EvolutionPlan* plan = active_plan_locked(component);
  if (plan != nullptr) {
    view.stage = plan->stage;
    view.plan = plan->id;
    view.new_writer_enabled = plan->new_writer_enabled;
    view.rollback_barrier_crossed = plan->rollback_barrier_crossed;
    view.epoch = plan->epoch;
    view.protocol_generation = plan->protocol_generation;
    view.schema_generation = plan->schema_generation;
  } else {
    const auto versions = durable_.components.versions_of(component);
    RuntimeGeneration best{};
    for (const auto* version : versions) {
      if (is_authoritative_lifecycle(version->lifecycle) && (!best.is_set() || version->generation > best)) {
        best = version->generation;
        view.protocol_generation = version->protocols.supported.highest();
        view.schema_generation = version->schemas.supported.highest();
        if (version->capability_generation.is_set()) {
          // capability generation is reported through the coexistence entries
        }
      }
    }
    view.stage = RolloutStage::None;
  }

  for (const auto* version : durable_.components.versions_of(component)) {
    GenerationPermission permission;
    permission.generation = version->generation;
    permission.lifecycle = version->lifecycle;
    permission.is_authoritative = view.has_authoritative && version->generation == view.authoritative;
    const OperationClass operation =
        derive_operation_class_locked(component, version->generation, plan != nullptr
                                                                          ? plan->protocol_generation
                                                                          : version->protocols.supported.highest());
    permission.operation = operation;
    permission.may_read = operation != OperationClass::None;
    permission.may_write = permits_mutation(operation);
    permission.protocol = plan != nullptr && version->generation == plan->candidate_generation
                              ? plan->protocol_generation
                              : version->protocols.supported.highest();
    permission.schema = plan != nullptr && version->generation == plan->candidate_generation
                            ? plan->schema_generation
                            : version->schemas.supported.highest();
    if (is_terminal_lifecycle(version->lifecycle)) {
      permission.may_read = false;
      permission.may_write = false;
      permission.operation = OperationClass::None;
      permission.reason = DetailText::from_valid("RETIRED: terminal for this runtime generation");
    } else if (version->generation == view.authoritative) {
      permission.reason = DetailText::from_valid("authoritative runtime generation");
    } else if (plan != nullptr && version->generation == plan->candidate_generation) {
      permission.reason = DetailText::from_valid("candidate generation under active evolution plan");
    } else {
      permission.reason = DetailText::from_valid("coexisting generation");
    }
    view.coexistence.push_back(permission);
  }
  std::sort(view.coexistence.begin(), view.coexistence.end(),
            [](const GenerationPermission& left, const GenerationPermission& right) {
              return left.generation < right.generation;
            });

  for (const auto* gate : durable_.gates.all()) {
    if (gate->enabled) view.enabled_features.push_back(gate->id);
  }

  view.evidence_current = true;
  if (plan != nullptr) {
    const CompatibilityEdge* edge = durable_.matrix.find(plan->current_generation, plan->candidate_generation);
    view.evidence_current = edge != nullptr && evidence_current_locked(*edge);
  }

  view.explanation.push_back(std::string("coordinator epoch ") +
                             std::to_string(durable_.coordinator_epoch.raw()));
  view.explanation.push_back(std::string("evolution epoch ") + std::to_string(view.epoch.raw()));
  if (view.has_authoritative) {
    view.explanation.push_back("authoritative runtime generation " +
                               std::to_string(view.authoritative.raw()));
  } else {
    view.explanation.push_back("no authoritative runtime generation is recorded for this component");
  }
  if (plan != nullptr) {
    view.explanation.push_back(std::string("rollout stage ") + to_string(plan->stage));
    view.explanation.push_back(std::string("new writer enabled: ") +
                               (plan->new_writer_enabled ? "yes" : "no"));
    if (plan->rollback_barrier_crossed) {
      view.explanation.push_back("rollback barrier has been crossed; rollback is no longer available");
    }
  } else {
    view.explanation.push_back("no active evolution plan for this component");
  }
  if (durable_.is_retired(component, view.authoritative)) {
    view.explanation.push_back("the authoritative generation is retired (inconsistent state)");
  }
  return view;
}

std::vector<LiveWorker> EvolutionCoordinator::live_workers() const {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  std::vector<LiveWorker> out;
  out.reserve(live_workers_.size());
  for (const auto& [id, worker] : live_workers_) {
    (void)id;
    out.push_back(worker);
  }
  return out;
}

DurableState EvolutionCoordinator::durable_state_copy() const {
  internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
  return durable_;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
CommandResult EvolutionCoordinator::command_hello(const ProtocolHandshake& handshake, PeerIdentity& peer) {
  CommandResult result;
  std::string encoded;
  Status persist_status = Status::ok();
  bool do_persist = false;

  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status valid = handshake.validate();
    if (valid.is_failure()) {
      ++stats_.handshakes_rejected;
      result.status = valid;
      result.json = "{}";
      return result;
    }
    if (durable_.is_retired(handshake.component, handshake.runtime_generation)) {
      ++stats_.handshakes_rejected;
      result.status = Status::failure(ErrorCode::RetiredGeneration,
                                      "runtime generation is retired and may not reconnect");
      result.explanation.push_back("retired runtime generation rejected before session creation");
      return result;
    }
    if (durable_.is_boot_fenced(handshake.worker, handshake.boot)) {
      ++stats_.handshakes_rejected;
      result.status = Status::failure(ErrorCode::StaleBoot, "boot identity has been fenced");
      result.explanation.push_back("fenced boot rejected before session creation");
      return result;
    }
    const auto worker_it = durable_.workers.find(handshake.worker);
    if (worker_it != durable_.workers.end()) {
      if (handshake.boot < worker_it->second.last_boot) {
        ++stats_.handshakes_rejected;
        result.status = Status::failure(ErrorCode::StaleBoot,
                                        "boot identity is older than the recorded incarnation");
        result.explanation.push_back("stale boot identity rejected");
        return result;
      }
      if (worker_it->second.fenced && handshake.boot == worker_it->second.last_boot) {
        ++stats_.handshakes_rejected;
        result.status = Status::failure(ErrorCode::StaleBoot, "recorded boot identity is fenced");
        result.explanation.push_back("fenced boot identity rejected");
        return result;
      }
    }

    // Protocol negotiation: never below the declared safety floor.
    ProtocolGenerationSet local_supported;
    for (const auto* descriptor : durable_.protocols.all()) {
      if (descriptor->id != handshake.protocol) continue;
      (void)local_supported.add(descriptor->generation);
    }
    if (local_supported.empty()) {
      ++stats_.handshakes_rejected;
      result.status = Status::failure(ErrorCode::NotFound, "peer requested an unknown protocol id");
      return result;
    }
    NegotiationRequest request;
    request.protocol = handshake.protocol;
    request.local_supported = local_supported;
    request.peer_supported = handshake.supported_protocols;
    request.required_minimum = handshake.required_minimum.is_set() ? handshake.required_minimum
                                                                  : ProtocolGeneration::from_raw(1);
    ProtocolNegotiator negotiator(&durable_.protocols);
    const NegotiationResult negotiation = negotiator.negotiate(request);
    if (!negotiation.is_ok()) {
      ++stats_.handshakes_rejected;
      result.status = negotiation.status;
      result.explanation = negotiation.explanation;
      result.json = "{}";
      return result;
    }

    const RuntimeComponentVersion* version =
        durable_.components.find(handshake.component, handshake.runtime_generation);
    const bool registered = version != nullptr;
    if (registered) {
      // The negotiated schema must be one the registered version actually supports.
      if (!version->schemas.readable.contains(handshake.committed_schema) &&
          handshake.committed_schema.is_set()) {
        ++stats_.handshakes_rejected;
        result.status = Status::failure(ErrorCode::Incompatible,
                                        "peer committed schema generation is not readable by its registered version");
        return result;
      }
    }

    peer.component = handshake.component;
    peer.version = handshake.version;
    peer.worker = handshake.worker;
    peer.boot = handshake.boot;
    peer.runtime_generation = handshake.runtime_generation;
    peer.protocol = handshake.protocol;
    peer.agreed_protocol = negotiation.chosen;
    peer.agreed_schema = handshake.committed_schema.is_set() ? handshake.committed_schema
                                                             : handshake.supported_schemas.highest();
    peer.capability_generation = handshake.capability_generation;
    peer.coordinator_epoch = durable_.coordinator_epoch;
    peer.epoch = epoch_for_locked(peer.component);
    peer.registered = registered;
    peer.operation = derive_operation_class_locked(peer.component, peer.runtime_generation,
                                                   peer.agreed_protocol);
    if (!registered) peer.operation = OperationClass::None;
    if (peer.is_operator) {
      // Administrative authority comes from the operator flag (loopback only),
      // not from a registered runtime generation.
      peer.operation = OperationClass::FullMutation;
    }

    LiveWorker worker;
    worker.worker = handshake.worker;
    worker.component = handshake.component;
    worker.generation = handshake.runtime_generation;
    worker.boot = handshake.boot;
    worker.capability_generation = handshake.capability_generation;
    worker.negotiated_protocol = negotiation.chosen;
    worker.committed_schema = peer.agreed_schema;
    worker.operation = peer.operation;
    worker.session = peer.session;
    worker.active = true;
    worker.frames = 0;
    worker.evidence = durable_.evidence_generation;
    live_workers_[worker.worker] = worker;
    worker_sessions_[worker.worker] = peer.session;

    WorkerRecord& record = durable_.workers[handshake.worker];
    record.worker = handshake.worker;
    record.component = handshake.component;
    record.generation = handshake.runtime_generation;
    record.last_boot = handshake.boot;
    record.fenced = false;
    record.drained = false;
    record.evidence = durable_.evidence_generation;
    record.detail = DetailText::from_valid("handshake accepted; authority requires revalidation");
    do_persist = true;
    ++stats_.handshakes_ok;

    JsonWriter writer;
    writer.begin_object();
    writer.field("status", "OK");
    writer.field("detail", "handshake accepted");
    writer.field_generation("negotiated_protocol", negotiation.chosen);
    writer.field_generation("negotiated_schema", peer.agreed_schema);
    writer.field("operation_class", to_string(peer.operation));
    writer.field_generation("evolution_epoch", peer.epoch);
    writer.field_generation("coordinator_epoch", peer.coordinator_epoch);
    writer.field_generation("stage_generation", durable_.stage_generation);
    writer.field_generation("gate_generation", durable_.gates.generation());
    writer.field_generation("matrix_generation", durable_.matrix.generation());
    writer.field("registered", registered);
    const EvolutionPlan* plan = active_plan_locked(peer.component);
    writer.field("new_writer_enabled", plan != nullptr && plan->new_writer_enabled);
    writer.field("rollout_stage", plan != nullptr ? to_string(plan->stage) : to_string(RolloutStage::None));
    const auto authoritative = durable_.components.authoritative_generation(peer.component);
    writer.field_generation("authoritative_generation",
                            authoritative.has_value() ? *authoritative : RuntimeGeneration::unset());
    writer.key("features");
    writer.begin_array();
    for (const auto* gate : durable_.gates.all()) {
      if (!gate->enabled) continue;
      writer.begin_object();
      writer.field("feature", gate->id.view());
      writer.field_generation("generation", gate->generation);
      writer.end_object();
    }
    writer.end_array();
    writer.key("explanation");
    writer.begin_array();
    for (const auto& line : negotiation.explanation) writer.string(line);
    writer.end_array();
    writer.end_object();
    result.json = writer.str();
    (void)result.fields.set_generation(FieldId::from_valid("negotiated_protocol"), negotiation.chosen);
    (void)result.fields.set_generation(FieldId::from_valid("negotiated_schema"), peer.agreed_schema);
    (void)result.fields.set_text(FieldId::from_valid("operation_class"), to_string(peer.operation));
    (void)result.fields.set_generation(FieldId::from_valid("evolution_epoch"), peer.epoch);
    (void)result.fields.set_generation(FieldId::from_valid("coordinator_epoch"), peer.coordinator_epoch);
    (void)result.fields.set_generation(FieldId::from_valid("stage_generation"), durable_.stage_generation);
    (void)result.fields.set_generation(FieldId::from_valid("gate_generation"), durable_.gates.generation());
    (void)result.fields.set_generation(FieldId::from_valid("matrix_generation"), durable_.matrix.generation());
    (void)result.fields.set_bool(FieldId::from_valid("registered"), registered);
    (void)result.fields.set_bool(FieldId::from_valid("new_writer_enabled"),
                                 plan != nullptr && plan->new_writer_enabled);
    (void)result.fields.set_generation(
        FieldId::from_valid("authoritative_generation"),
        authoritative.has_value() ? *authoritative : RuntimeGeneration::unset());
    if (do_persist) persist_status = encode_locked(encoded);
  }

  result.status = Status::ok();
  if (persist_status.is_failure()) return result;
  if (do_persist) {
    const Status stored = persist(encoded);
    if (stored.is_failure()) {
      result.status = stored;
      result.explanation.push_back("handshake accepted but durable state could not be persisted");
    }
  }
  return result;
}

}  // namespace ref