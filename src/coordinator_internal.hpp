// Runtime Evolution Fabric - internal shared helpers for the coordinator
// translation units. Not installed: this header is an implementation detail.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ref/coordinator.hpp"

namespace ref {
namespace internal {

// Identity of the built-in wire protocol, the built-in component state schema
// and the built-in feature gates.
[[nodiscard]] const ProtocolId& wire_protocol_id();
[[nodiscard]] const SchemaId& component_state_schema_id();
[[nodiscard]] const FeatureGateId& token_transform_feature_id();
[[nodiscard]] const FeatureGateId& shadow_query_feature_id();

// Field identities are built on demand: no shared cache, so no lock is ever
// taken while another lock is held.
[[nodiscard]] FieldId field_id(const char* name);


// Debug-only reentrancy detector for the coordinator state lock. Taking the
// state lock twice on one thread is a defect, not a supported pattern; in debug
// builds it aborts with a diagnostic instead of deadlocking or corrupting.
class StateLockGuard {
 public:
  StateLockGuard(std::mutex& mutex, std::atomic<std::thread::id>& owner);
  ~StateLockGuard();
  StateLockGuard(const StateLockGuard&) = delete;
  StateLockGuard& operator=(const StateLockGuard&) = delete;
  [[nodiscard]] std::mutex& mutex() noexcept { return *mutex_; }

 private:
  std::mutex* mutex_;
  std::atomic<std::thread::id>* owner_;
};

[[nodiscard]] CommandResult ok_result(std::string_view detail);
[[nodiscard]] CommandResult ok_json(std::string json, std::string note);
[[nodiscard]] CommandResult failure_result(const Status& status);

}  // namespace internal
}  // namespace ref
