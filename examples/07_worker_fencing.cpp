// Example 7: worker fencing over real TCP.
//
// Starts a coordinator in this process, connects a client, fences its boot
// identity, and shows that the fenced incarnation can no longer participate.
#include <cstdio>
#include <memory>

#include "ref/ref.hpp"

using namespace ref;

namespace {

ProtocolHandshake handshake_for(const char* worker, std::uint64_t boot) {
  ProtocolHandshake handshake;
  handshake.component = RuntimeComponentId::from_valid("orders");
  handshake.version.number = VersionNumber{1, 0, 0};
  handshake.version.artifact.build = BuildId::from_valid("orders-1.0.0");
  handshake.runtime_generation = RuntimeGeneration::from_raw(1);
  handshake.worker = WorkerId::from_valid(worker);
  handshake.boot = WorkerBootId::from_raw(boot);
  handshake.protocol = ProtocolId::from_valid("ref-wire");
  (void)handshake.supported_protocols.add(ProtocolGeneration::from_raw(1));
  handshake.required_minimum = ProtocolGeneration::from_raw(1);
  (void)handshake.supported_schemas.add(SchemaGeneration::from_raw(1));
  handshake.committed_schema = SchemaGeneration::from_raw(1);
  handshake.capability_generation = CapabilityGeneration::from_raw(1);
  handshake.declared_role = NoteText::from_valid("example worker");
  return handshake;
}

}  // namespace

int main() {
  EvolutionCoordinator coordinator(CoordinatorConfig{});
  if (coordinator.start().is_failure()) return 1;
  const PeerIdentity admin = coordinator.operator_identity().peer;

  RuntimeComponentVersion version;
  version.component = RuntimeComponentId::from_valid("orders");
  version.version.number = VersionNumber{1, 0, 0};
  version.version.artifact.build = BuildId::from_valid("orders-1.0.0");
  version.generation = RuntimeGeneration::from_raw(1);
  (void)version.protocols.supported.add(ProtocolGeneration::from_raw(1));
  version.protocols.readable = version.protocols.supported;
  version.protocols.writable = version.protocols.supported;
  version.protocols.minimum_safety = ProtocolGeneration::from_raw(1);
  (void)version.schemas.supported.add(SchemaGeneration::from_raw(1));
  version.schemas.readable = version.schemas.supported;
  version.schemas.writable = version.schemas.supported;
  (void)version.schemas.readable_formats.add(kStateFormatGenerationV1);
  (void)version.schemas.writable_formats.add(kStateFormatGenerationV1);
  version.capability_generation = CapabilityGeneration::from_raw(1);
  version.provenance = EvidenceClass::Real;
  version.lifecycle = LifecycleState::Current;
  (void)coordinator.command_register_component(admin, version);

  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = coordinator.port();

  EvolutionClient first(config);
  HandshakeOutcome outcome;
  std::printf("handshake: %s protocol=%llu operation=%s\n",
              first.connect(handshake_for("worker-a", 1), outcome).to_string().c_str(),
              static_cast<unsigned long long>(outcome.negotiated_protocol.raw()),
              to_string(outcome.operation));

  std::printf("fence boot 1: %s\n",
              coordinator
                  .command_fence(admin, WorkerId::from_valid("worker-a"), WorkerBootId::from_raw(1),
                                 "operator fence")
                  .status.to_string()
                  .c_str());

  EvolutionClient stale(config);
  const Status refused = stale.connect(handshake_for("worker-a", 1), outcome);
  std::printf("fenced incarnation reconnecting: %s\n", refused.to_string().c_str());

  EvolutionClient replacement(config);
  const Status accepted = replacement.connect(handshake_for("worker-a", 2), outcome);
  std::printf("replacement incarnation (boot 2): %s operation=%s\n", accepted.to_string().c_str(),
              to_string(outcome.operation));
  coordinator.stop();
  return 0;
}
