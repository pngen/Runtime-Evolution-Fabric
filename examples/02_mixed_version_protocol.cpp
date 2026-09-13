// Example 2: mixed-version protocol negotiation.
//
// Shows: generation 2 supporting protocols 1 and 2, the mixed-version phase
// negotiating protocol 1, and the newer message type being unavailable until
// both peers can speak protocol 2.
#include <cstdio>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  ProtocolRegistry registry;
  (void)registry.publish(build_wire_protocol(ProtocolGeneration::from_raw(1)));
  (void)registry.publish(build_wire_protocol(ProtocolGeneration::from_raw(2)));
  ProtocolNegotiator negotiator(&registry);

  NegotiationRequest request;
  request.protocol = ProtocolId::from_valid("ref-wire");
  (void)request.local_supported.add(ProtocolGeneration::from_raw(1));
  (void)request.local_supported.add(ProtocolGeneration::from_raw(2));
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(1));
  request.required_minimum = ProtocolGeneration::from_raw(1);

  const NegotiationResult mixed = negotiator.negotiate(request);
  std::printf("mixed-version negotiation: %s chosen=%llu class=%s\n", mixed.status.to_string().c_str(),
              static_cast<unsigned long long>(mixed.chosen.raw()), to_string(mixed.operation_class));

  (void)request.peer_supported.add(ProtocolGeneration::from_raw(2));
  const NegotiationResult full = negotiator.negotiate(request);
  std::printf("both sides at protocol 2: signed=%llu class=%s\n",
              static_cast<unsigned long long>(full.chosen.raw()), to_string(full.operation_class));

  const ProtocolDescriptor v1 = build_wire_protocol(ProtocolGeneration::from_raw(1));
  const ProtocolDescriptor v2 = build_wire_protocol(ProtocolGeneration::from_raw(2));
  const MessageTypeId transform = MessageTypeId::from_valid("REQUEST_STATE_TRANSFORM");
  std::printf("protocol 1 supports the transform message: %s\n",
              v1.supports_message(transform) ? "yes" : "no");
  std::printf("protocol 2 supports the transform message: %s\n",
              v2.supports_message(transform) ? "yes" : "no");

  // A peer that cannot satisfy the safety floor is refused rather than
  // silently downgraded.
  request.required_minimum = ProtocolGeneration::from_raw(2);
  request.peer_supported = ProtocolGenerationSet{};
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(1));
  const NegotiationResult refused = negotiator.negotiate(request);
  std::printf("below the safety floor: %s / %s\n", refused.status.to_string().c_str(),
              to_string(refused.outcome));
  return 0;
}
