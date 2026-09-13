#include "coordinator_internal.hpp"

#include <cstdio>
#include <cstdlib>

namespace ref {
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