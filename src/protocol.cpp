#include "ref/protocol.hpp"

#include <algorithm>
#include <array>

namespace ref {
namespace {

constexpr std::size_t kMaxMessagesPerProtocol = limits::kMessageTypes;

}  // namespace

const char* to_string(UnknownFieldPolicy policy) noexcept {
  switch (policy) {
    case UnknownFieldPolicy::Reject: return "REJECT";
    case UnknownFieldPolicy::Ignore: return "IGNORE";
    case UnknownFieldPolicy::Preserve: return "PRESERVE";
  }
  return "REJECT";
}

const char* to_string(UnknownMessagePolicy policy) noexcept {
  switch (policy) {
    case UnknownMessagePolicy::Reject: return "REJECT";
    case UnknownMessagePolicy::Ignore: return "IGNORE";
  }
  return "REJECT";
}

const char* to_string(OperationClass value) noexcept {
  switch (value) {
    case OperationClass::None: return "NONE";
    case OperationClass::ReadOnly: return "READ_ONLY";
    case OperationClass::RestrictedMutation: return "RESTRICTED_MUTATION";
    case OperationClass::FullMutation: return "FULL_MUTATION";
  }
  return "NONE";
}

bool permits_mutation(OperationClass value) noexcept {
  return value == OperationClass::RestrictedMutation || value == OperationClass::FullMutation;
}

std::optional<OperationClass> parse_operation_class(std::string_view text) noexcept {
  if (text == "NONE") return OperationClass::None;
  if (text == "READ_ONLY") return OperationClass::ReadOnly;
  if (text == "RESTRICTED_MUTATION") return OperationClass::RestrictedMutation;
  if (text == "FULL_MUTATION") return OperationClass::FullMutation;
  return std::nullopt;
}

Status ProtocolDescriptor::validate() const noexcept {
  if (id.empty()) return Status::failure(ErrorCode::InvalidArgument, "protocol id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "protocol generation is unset");
  if (!minimum_safety_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "protocol minimum safety generation is unset");
  }
  if (minimum_safety_generation > generation) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "protocol minimum safety generation exceeds the protocol generation");
  }
  if (messages.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "protocol declares no message types");
  }
  if (messages.size() > kMaxMessagesPerProtocol) {
    return Status::failure(ErrorCode::LimitExceeded, "protocol declares too many message types");
  }
  for (std::size_t i = 0; i < messages.size(); ++i) {
    const auto& message = messages[i];
    if (message.id.empty()) {
      return Status::failure(ErrorCode::InvalidArgument, "message type id is empty");
    }
    if (!message.introduced_in.is_set() || message.introduced_in > generation) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "message type introduced at a generation beyond the protocol");
    }
    if (message.fields.size() > limits::kMessageFields) {
      return Status::failure(ErrorCode::LimitExceeded, "message type declares too many fields");
    }
    for (const auto& field : message.fields) {
      if (field.id.empty()) return Status::failure(ErrorCode::InvalidArgument, "field id is empty");
      if (!field.introduced_in.is_set() || field.introduced_in > generation) {
        return Status::failure(ErrorCode::InvalidArgument, "field introduced beyond the protocol generation");
      }
    }
    for (std::size_t j = i + 1; j < messages.size(); ++j) {
      if (messages[j].id == message.id) {
        return Status::failure(ErrorCode::InvalidArgument, "duplicate message type in protocol");
      }
    }
  }
  if (provenance == EvidenceClass::Unknown) {
    return Status::failure(ErrorCode::InvalidArgument, "protocol descriptor has no evidence provenance");
  }
  return Status::ok();
}

const MessageTypeDescriptor* ProtocolDescriptor::find_message(const MessageTypeId& message) const noexcept {
  for (const auto& entry : messages) {
    if (entry.id == message) return &entry;
  }
  return nullptr;
}

bool ProtocolDescriptor::supports_message(const MessageTypeId& message) const noexcept {
  const MessageTypeDescriptor* descriptor = find_message(message);
  if (descriptor == nullptr) return false;
  return descriptor->introduced_in <= generation;
}

bool ProtocolDescriptor::supports_mutating(const MessageTypeId& message) const noexcept {
  const MessageTypeDescriptor* descriptor = find_message(message);
  if (descriptor == nullptr) return false;
  return descriptor->mutating && descriptor->introduced_in <= generation;
}

Status ProtocolRegistry::publish(const ProtocolDescriptor& descriptor) {
  const Status valid = descriptor.validate();
  if (valid.is_failure()) return valid;
  const auto key = std::make_pair(descriptor.id, descriptor.generation.raw());
  const auto existing = by_id_.find(key);
  if (existing != by_id_.end()) {
    if (existing->second.minimum_safety_generation != descriptor.minimum_safety_generation) {
      return Status::failure(ErrorCode::Conflict,
                             "protocol generation already published with a different safety floor");
    }
    existing->second = descriptor;
    return Status::ok();
  }
  if (by_id_.size() >= limits::kProtocolGenerations) {
    return Status::failure(ErrorCode::LimitExceeded, "protocol generation bound reached");
  }
  by_id_.emplace(key, descriptor);
  return Status::ok();
}

const ProtocolDescriptor* ProtocolRegistry::find(const ProtocolId& id,
                                                 ProtocolGeneration generation) const noexcept {
  const auto it = by_id_.find(std::make_pair(id, generation.raw()));
  return it == by_id_.end() ? nullptr : &it->second;
}

ProtocolGeneration ProtocolRegistry::highest(const ProtocolId& id) const noexcept {
  ProtocolGeneration best{};
  for (const auto& [key, descriptor] : by_id_) {
    if (key.first != id) continue;
    if (!best.is_set() || descriptor.generation > best) best = descriptor.generation;
  }
  return best;
}

ProtocolGeneration ProtocolRegistry::lowest(const ProtocolId& id) const noexcept {
  ProtocolGeneration best{};
  for (const auto& [key, descriptor] : by_id_) {
    if (key.first != id) continue;
    if (!best.is_set() || descriptor.generation < best) best = descriptor.generation;
  }
  return best;
}

std::vector<const ProtocolDescriptor*> ProtocolRegistry::all() const {
  std::vector<const ProtocolDescriptor*> out;
  out.reserve(by_id_.size());
  for (const auto& [key, descriptor] : by_id_) {
    (void)key;
    out.push_back(&descriptor);
  }
  return out;
}

Status ProtocolRegistry::validate() const {
  if (by_id_.size() > limits::kProtocolGenerations) {
    return Status::failure(ErrorCode::Corrupt, "protocol registry exceeds the generation bound");
  }
  for (const auto& [key, descriptor] : by_id_) {
    if (key.first != descriptor.id || key.second != descriptor.generation.raw()) {
      return Status::failure(ErrorCode::Corrupt, "protocol registry key does not match its descriptor");
    }
    const Status valid = descriptor.validate();
    if (valid.is_failure()) return valid;
  }
  return Status::ok();
}

NegotiationResult ProtocolNegotiator::negotiate(const NegotiationRequest& request) const {
  NegotiationResult result;
  result.peer_highest = request.peer_supported.highest();

  if (registry_ == nullptr) {
    result.status = Status::failure(ErrorCode::Internal, "protocol negotiator has no registry");
    return result;
  }
  if (request.protocol.empty()) {
    result.status = Status::failure(ErrorCode::InvalidArgument, "negotiation requires a protocol id");
    return result;
  }
  if (request.local_supported.empty() || request.peer_supported.empty()) {
    result.status = Status::failure(ErrorCode::Incompatible, "empty protocol support set");
    result.outcome = CompatOutcome::IncompatibleProtocol;
    result.explanation.push_back("one side declares no protocol generation");
    return result;
  }

  // Highest common generation that satisfies every declared requirement.
  ProtocolGeneration chosen{};
  bool downgraded = false;
  const ProtocolGeneration ceiling = request.preferred.is_set()
                                         ? std::min(request.preferred, request.local_supported.highest())
                                         : request.local_supported.highest();

  for (std::uint8_t i = 0; i < request.local_supported.size(); ++i) {
    const ProtocolGeneration candidate = request.local_supported.at(i);
    if (candidate > ceiling) continue;
    if (!request.peer_supported.contains(candidate)) continue;
    if (request.required_minimum.is_set() && candidate < request.required_minimum) continue;
    const ProtocolDescriptor* descriptor = registry_->find(request.protocol, candidate);
    if (descriptor == nullptr) continue;  // unregistered generation is never negotiable
    bool satisfies = true;
    for (const auto& message : request.required_messages) {
      if (!descriptor->supports_message(message)) {
        satisfies = false;
        break;
      }
    }
    if (satisfies) {
      for (const auto& message : request.required_mutating_messages) {
        if (!descriptor->supports_mutating(message)) {
          satisfies = false;
          break;
        }
      }
    }
    if (!satisfies) continue;
    chosen = candidate;
    break;
  }

  if (!chosen.is_set()) {
    const bool any_common = [&] {
      for (std::uint8_t i = 0; i < request.local_supported.size(); ++i) {
        if (request.peer_supported.contains(request.local_supported.at(i))) return true;
      }
      return false;
    }();
    if (any_common) {
      result.status = Status::failure(
          ErrorCode::Incompatible,
          "no common protocol generation satisfies the required messages and safety floor");
      result.outcome = CompatOutcome::IncompatibleProtocol;
      result.explanation.push_back("common generations exist but none satisfies required semantics");
    } else {
      result.status =
          Status::failure(ErrorCode::Incompatible, "no common protocol generation between peers");
      result.outcome = CompatOutcome::IncompatibleProtocol;
      result.explanation.push_back("disjoint protocol support sets");
    }
    return result;
  }

  const ProtocolDescriptor* descriptor = registry_->find(request.protocol, chosen);
  if (descriptor == nullptr) {
    result.status = Status::failure(ErrorCode::Internal, "negotiated generation vanished from the registry");
    return result;
  }

  const ProtocolGeneration highest_common =
      std::min(request.local_supported.highest(), request.peer_supported.highest());
  downgraded = chosen < highest_common;

  OperationClass operation = OperationClass::ReadOnly;
  if (!descriptor->messages.empty()) {
    operation = downgraded ? OperationClass::RestrictedMutation : OperationClass::FullMutation;
  }
  if (request.require_mutation && !permits_mutation(operation)) {
    result.status = Status::failure(ErrorCode::Incompatible,
                                    "peer cannot satisfy the required mutation semantics");
    result.outcome = CompatOutcome::IncompatibleProtocol;
    result.explanation.push_back("mutation required but the negotiated generation offers no mutating message");
    return result;
  }

  result.chosen = chosen;
  result.operation_class = operation;
  result.downgraded = downgraded;
  result.outcome = downgraded ? CompatOutcome::CompatibleAfterProtocolNegotiation
                              : CompatOutcome::FullyCompatible;
  result.status = Status::ok();
  result.explanation.push_back("negotiated protocol generation " + std::to_string(chosen.raw()) +
                               (downgraded ? " (downgraded from " +
                                                 std::to_string(highest_common.raw()) + ")"
                                           : " (highest common)"));
  result.explanation.push_back(std::string("operation class ") + to_string(operation));
  return result;
}

Status ProtocolHandshake::validate() const noexcept {
  const Status version_status = [&] {
    if (!version.is_set()) {
      return Status::failure(ErrorCode::InvalidArgument, "handshake version identity is unset");
    }
    return Status::ok();
  }();
  if (version_status.is_failure()) return version_status;
  if (component.empty()) return Status::failure(ErrorCode::InvalidArgument, "handshake component is empty");
  if (!runtime_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "handshake runtime generation is unset");
  }
  if (!boot.is_set()) return Status::failure(ErrorCode::InvalidArgument, "handshake boot identity is unset");
  if (worker.empty()) return Status::failure(ErrorCode::InvalidArgument, "handshake worker identity is empty");
  if (protocol.empty()) return Status::failure(ErrorCode::InvalidArgument, "handshake protocol id is empty");
  if (supported_protocols.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "handshake declares no supported protocol generation");
  }
  if (supported_schemas.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "handshake declares no supported schema generation");
  }
  if (committed_schema.is_set() && !supported_schemas.contains(committed_schema)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "handshake committed schema generation is not in the supported set");
  }
  if (!capability_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "handshake capability generation is unset");
  }
  return Status::ok();
}

}  // namespace ref
