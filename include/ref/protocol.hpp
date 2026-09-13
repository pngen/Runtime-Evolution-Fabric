// Runtime Evolution Fabric - protocol generations, descriptors and negotiation.
//
// Parsing a frame is not permission to mutate. Negotiation therefore returns an
// operation class alongside the chosen generation, and never silently drops
// below the minimum safety contract declared by policy.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/compat.hpp"
#include "ref/component.hpp"
#include "ref/ids.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Message and field descriptors
// ---------------------------------------------------------------------------
struct FieldDescriptor {
  FieldId id{};
  ProtocolGeneration introduced_in{};
  bool required{true};
};

struct MessageTypeDescriptor {
  MessageTypeId id{};
  ProtocolGeneration introduced_in{};
  bool mutating{false};      // changes authoritative state when accepted
  bool state_replacing{false};
  std::vector<FieldDescriptor> fields{};
};

enum class UnknownFieldPolicy : std::uint8_t { Reject = 0, Ignore, Preserve };
enum class UnknownMessagePolicy : std::uint8_t { Reject = 0, Ignore };

[[nodiscard]] const char* to_string(UnknownFieldPolicy policy) noexcept;
[[nodiscard]] const char* to_string(UnknownMessagePolicy policy) noexcept;

struct ProtocolDescriptor {
  ProtocolId id{};
  ProtocolGeneration generation{};
  // A peer negotiating this generation must be at or above this floor for
  // mutation authority; negotiation never goes below it.
  ProtocolGeneration minimum_safety_generation{};
  std::vector<MessageTypeDescriptor> messages{};
  UnknownFieldPolicy unknown_fields{UnknownFieldPolicy::Reject};
  UnknownMessagePolicy unknown_messages{UnknownMessagePolicy::Reject};
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration evidence{};
  DetailText notes{};

  [[nodiscard]] Status validate() const noexcept;
  [[nodiscard]] const MessageTypeDescriptor* find_message(const MessageTypeId& message) const noexcept;
  // A holder of 'reader' can decode a message emitted at this descriptor's
  // generation when the message exists in both and every field the reader
  // requires is present in the writer's form.
  [[nodiscard]] bool supports_message(const MessageTypeId& message) const noexcept;
  [[nodiscard]] bool supports_mutating(const MessageTypeId& message) const noexcept;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
class ProtocolRegistry {
 public:
  ProtocolRegistry() = default;

  Status publish(const ProtocolDescriptor& descriptor);
  [[nodiscard]] const ProtocolDescriptor* find(const ProtocolId& id,
                                               ProtocolGeneration generation) const noexcept;
  [[nodiscard]] ProtocolGeneration highest(const ProtocolId& id) const noexcept;
  [[nodiscard]] ProtocolGeneration lowest(const ProtocolId& id) const noexcept;
  [[nodiscard]] std::vector<const ProtocolDescriptor*> all() const;
  [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }
  [[nodiscard]] Status validate() const;

 private:
  std::map<std::pair<ProtocolId, std::uint64_t>, ProtocolDescriptor> by_id_{};
};

// ---------------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------------
enum class OperationClass : std::uint8_t {
  None = 0,          // no control channel
  ReadOnly,          // queries and shadow processing only
  RestrictedMutation, // mutation limited to the negotiated (possibly older) form
  FullMutation,      // full mutation authority for the negotiated generation
};

[[nodiscard]] const char* to_string(OperationClass value) noexcept;
[[nodiscard]] bool permits_mutation(OperationClass value) noexcept;
[[nodiscard]] std::optional<OperationClass> parse_operation_class(std::string_view text) noexcept;

struct NegotiationRequest {
  ProtocolId protocol{};
  ProtocolGenerationSet local_supported{};
  ProtocolGenerationSet peer_supported{};
  ProtocolGeneration required_minimum{};   // policy floor
  ProtocolGeneration preferred{};          // unset => highest common
  std::vector<MessageTypeId> required_messages{};
  std::vector<MessageTypeId> required_mutating_messages{};
  bool require_mutation{false};
};

struct NegotiationResult {
  Status status{};
  ProtocolGeneration chosen{};
  ProtocolGeneration peer_highest{};
  OperationClass operation_class{OperationClass::None};
  CompatOutcome outcome{CompatOutcome::Unknown};
  bool downgraded{false};
  std::vector<std::string> explanation{};

  [[nodiscard]] bool is_ok() const noexcept { return status.is_ok(); }
};

class ProtocolNegotiator {
 public:
  explicit ProtocolNegotiator(const ProtocolRegistry* registry) noexcept : registry_(registry) {}

  [[nodiscard]] NegotiationResult negotiate(const NegotiationRequest& request) const;

 private:
  const ProtocolRegistry* registry_;
};

// ---------------------------------------------------------------------------
// Handshake description exchanged during connection establishment
// ---------------------------------------------------------------------------
struct ProtocolHandshake {
  RuntimeComponentId component{};
  RuntimeVersionId version{};
  RuntimeGeneration runtime_generation{};
  WorkerId worker{};
  WorkerBootId boot{};
  ProtocolId protocol{};
  ProtocolGenerationSet supported_protocols{};
  ProtocolGeneration required_minimum{};
  SchemaGenerationSet supported_schemas{};
  SchemaGeneration committed_schema{};
  CapabilityGeneration capability_generation{};
  EvolutionEpoch evolution_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  std::vector<FeatureSupport> features{};
  DetailText declared_role{};

  [[nodiscard]] Status validate() const noexcept;
};

}  // namespace ref
