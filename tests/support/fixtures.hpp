// Runtime Evolution Fabric - deterministic test fixtures.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ref/ref.hpp"

namespace reftest {

// Temporary directory that removes itself, so tests never leave debris behind.
class TempDir {
 public:
  explicit TempDir(const std::string& name);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;

 private:
  std::string path_;
};

// Deterministic runtime component version.
[[nodiscard]] ref::RuntimeComponentVersion make_component(
    const char* component, std::uint64_t generation, const char* protocols, const char* schemas,
    ref::LifecycleState lifecycle,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& migrations = {},
    const std::vector<ref::FeatureSupport>& features = {},
    const char* artifact = "refworker");

// Compatibility edge with an explicit profile: "compatible" (permissive) or
// "mixed-version" (permissive with negotiation and read-only writes).
[[nodiscard]] ref::CompatibilityEdge make_edge(std::uint64_t from, std::uint64_t to,
                                               const char* profile);

// Built-in protocol and schema registries as the coordinator installs them.
[[nodiscard]] ref::ProtocolRegistry make_protocol_registry();
[[nodiscard]] ref::SchemaRegistry make_schema_registry();

// Operator identity for in-process command calls.
[[nodiscard]] ref::PeerIdentity operator_peer();

[[nodiscard]] ref::FeatureSupport feature(const char* id, std::uint64_t generation, bool publish,
                                          bool consume);

// Creates a fresh state file for a worker and returns its path.
[[nodiscard]] ref::Status make_worker_state(const std::string& path, const char* owner,
                                            std::uint64_t counter);

}  // namespace reftest
