#include "coordinator_internal.hpp"

#include <cstdio>
#include <cstdlib>

namespace ref {

EvolutionCoordinator::CommandScope::CommandScope(EvolutionCoordinator& coordinator)
    : coordinator_(&coordinator), serial_(coordinator.command_mutex_) {}

void EvolutionCoordinator::CommandScope::begin_locked() {
  durable_ = coordinator_->durable_;
  pending_actions_ = coordinator_->pending_actions_;
  armed_ = true;
}

EvolutionCoordinator::CommandScope::~CommandScope() {
  if (!armed_ || committed_) return;
  // Only commands write the durable state and the action ledger, and no other
  // command can have run since begin_locked(), so putting both back loses
  // nothing. Session authority is derived from the durable state and follows it.
  internal::StateLockGuard guard(coordinator_->state_mutex_, coordinator_->state_lock_owner_);
  coordinator_->durable_ = std::move(durable_);
  coordinator_->pending_actions_ = std::move(pending_actions_);
  coordinator_->refresh_all_worker_authority_locked();
}

namespace internal {
const ProtocolId& wire_protocol_id() {
  static const ProtocolId id = ProtocolId::from_valid("ref-wire");
  return id;
}

const SchemaId& component_state_schema_id() {
  static const SchemaId id = SchemaId::from_valid("component-state");
  return id;
}

const FeatureGateId& token_transform_feature_id() {
  static const FeatureGateId id = FeatureGateId::from_valid("state-token-transform");
  return id;
}

const FeatureGateId& shadow_query_feature_id() {
  static const FeatureGateId id = FeatureGateId::from_valid("shadow-query");
  return id;
}

FieldId field_id(const char* name) { return FieldId::from_valid(name); }

StateLockGuard::StateLockGuard(std::mutex& mutex, std::atomic<std::thread::id>& owner)
    : mutex_(&mutex), owner_(&owner) {
#ifndef NDEBUG
  const std::thread::id self = std::this_thread::get_id();
  if (owner_->load() == self) {
    std::fprintf(stderr, "ref: reentrant acquisition of the coordinator state lock detected\n");
    std::abort();
  }
  mutex_->lock();
  owner_->store(self);
#else
  mutex_->lock();
  (void)owner_;
#endif
}

StateLockGuard::~StateLockGuard() {
#ifndef NDEBUG
  owner_->store(std::thread::id{});
#endif
  mutex_->unlock();
}

CommandResult ok_result(std::string_view detail) {
  CommandResult result;
  result.status = Status::ok();
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field("detail", detail);
  writer.end_object();
  result.json = writer.str();
  result.explanation.push_back(std::string(detail));
  return result;
}

CommandResult ok_json(std::string json, std::string note) {
  CommandResult result;
  result.status = Status::ok();
  result.json = std::move(json);
  result.explanation.push_back(std::move(note));
  return result;
}

CommandResult failure_result(const Status& status) {
  CommandResult result;
  result.status = status;
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", to_string(status.code()));
  writer.field("detail", status.detail());
  writer.end_object();
  result.json = writer.str();
  result.explanation.push_back(status.to_string());
  return result;
}

}  // namespace internal
}  // namespace ref