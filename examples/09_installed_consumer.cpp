// Example 9: an installed consumer of the package.
//
// This example is compiled against the installed RuntimeEvolutionFabric
// package through find_package, exactly like a downstream project would be.
// It uses only public headers and the exported target
// SummonSoftwareLabs::RuntimeEvolutionFabric.
#include <cstdio>
#include <string>

#include "ref/ref.hpp"

int main() {
  std::printf("Runtime Evolution Fabric %s\n", REF_FABRIC_VERSION);

  // Durable state round trip through the public API.
  ref::DurableState state;
  state.coordinator_epoch = ref::CoordinatorEpoch::first();
  state.evolution_epoch = ref::EvolutionEpoch::first();
  state.evidence_generation = ref::EvidenceGeneration::first();
  state.stage_generation = ref::StageGeneration::first();
  state.policy_generation = ref::PolicyGeneration::first();
  state.writer_version.number = ref::VersionNumber{1, 0, 0};
  state.writer_version.artifact.build = ref::BuildId::from_valid("consumer");
  if (ref::install_builtin_state_schemas(state.schemas).is_failure()) return 1;
  if (state.protocols.publish(ref::build_wire_protocol(ref::ProtocolGeneration::from_raw(1))).is_failure()) {
    return 1;
  }
  std::string encoded;
  const ref::Status encoded_status = ref::DurableStore::encode(state, encoded);
  if (encoded_status.is_failure()) {
    std::printf("encode failed: %s\n", encoded_status.to_string().c_str());
    return 1;
  }
  ref::DurableState loaded;
  const ref::Status decoded_status = ref::DurableStore::decode(encoded, loaded);
  std::printf("durable state encode/decode: %s (%zu bytes, %zu schemas, %zu protocols)\n",
              decoded_status.to_string().c_str(), encoded.size(), loaded.schemas.size(),
              loaded.protocols.size());

  // Compatibility facts through the public API.
  ref::CompatibilityMatrix matrix;
  ref::CompatibilityEdge edge;
  edge.from = ref::RuntimeGeneration::from_raw(1);
  edge.to = ref::RuntimeGeneration::from_raw(2);
  for (std::size_t i = 0; i < ref::kCompatAspectCount; ++i) {
    edge.aspects[i].outcome = ref::CompatOutcome::FullyCompatible;
    edge.aspects[i].provenance = ref::EvidenceClass::Real;
    edge.aspects[i].evidence = ref::EvidenceGeneration::first();
  }
  edge.permissions = ref::derive_permissions(edge);
  edge.generation = ref::CompatibilityGeneration::first();
  const ref::Status upserted = matrix.upsert(edge);
  std::printf("compatibility edge: %s permissions=%s\n", upserted.to_string().c_str(),
              ref::describe_permissions(edge.permissions).c_str());

  // Real coordinator + client over TCP.
  ref::CoordinatorConfig config;
  config.port = 0;
  ref::EvolutionCoordinator coordinator(config);
  const ref::Status started = coordinator.start();
  if (started.is_failure()) {
    std::printf("coordinator start failed: %s\n", started.to_string().c_str());
    return 1;
  }
  ref::ClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = coordinator.port();
  client_config.operator_mode = true;
  ref::EvolutionClient client(client_config);
  ref::ProtocolHandshake handshake;
  handshake.component = ref::RuntimeComponentId::from_valid("consumer");
  handshake.version.number = ref::VersionNumber{1, 0, 0};
  handshake.version.artifact.build = ref::BuildId::from_valid("consumer");
  handshake.runtime_generation = ref::RuntimeGeneration::from_raw(1);
  handshake.worker = ref::WorkerId::from_valid("consumer");
  handshake.boot = ref::WorkerBootId::from_raw(1);
  handshake.protocol = ref::ProtocolId::from_valid("ref-wire");
  (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(1));
  handshake.required_minimum = ref::ProtocolGeneration::from_raw(1);
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(1));
  handshake.committed_schema = ref::SchemaGeneration::from_raw(1);
  handshake.capability_generation = ref::CapabilityGeneration::from_raw(1);
  ref::HandshakeOutcome outcome;
  const ref::Status connected = client.connect(handshake, outcome);
  std::printf("coordinator handshake: %s negotiated protocol=%llu operation=%s\n",
              connected.to_string().c_str(),
              static_cast<unsigned long long>(outcome.negotiated_protocol.raw()),
              ref::to_string(outcome.operation));

  ref::WireMessage query;
  (void)query.set_text(ref::FieldId::from_valid("query"), "epochs");
  ref::Frame response;
  ref::WireMessage body;
  if (client.request(ref::MessageType::QueryState, query, response, body).is_ok()) {
    const auto json = body.document(ref::FieldId::from_valid("json"));
    std::printf("coordinator epochs: %s\n", json.has_value() ? std::string(*json).c_str() : "(none)");
  }
  coordinator.stop();
  return 0;
}
