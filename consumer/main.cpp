// Independent downstream consumer.
//
// This program is built only against the installed Runtime Evolution Fabric
// package: it includes the public headers, links the exported target and drives
// a real coordinator over real TCP. It proves the package is usable outside the
// source tree.
#include <cstdio>
#include <string>

#include "ref/ref.hpp"

int main() {
  std::printf("installed Runtime Evolution Fabric %s\n", REF_FABRIC_VERSION);

  ref::CoordinatorConfig config;
  config.port = 0;
  ref::EvolutionCoordinator coordinator(config);
  const ref::Status started = coordinator.start();
  if (started.is_failure()) {
    std::printf("coordinator start failed: %s\n", started.to_string().c_str());
    return 1;
  }

  // Everything below travels over a real socket to the coordinator process that
  // lives in this executable.
  ref::ClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = coordinator.port();
  client_config.operator_mode = true;
  ref::EvolutionClient client(client_config);

  ref::ProtocolHandshake handshake;
  handshake.component = ref::RuntimeComponentId::from_valid("consumer-component");
  handshake.version.number = ref::VersionNumber{1, 0, 0};
  handshake.version.artifact.build = ref::BuildId::from_valid("consumer-1.0.0");
  handshake.runtime_generation = ref::RuntimeGeneration::from_raw(1);
  handshake.worker = ref::WorkerId::from_valid("consumer-worker");
  handshake.boot = ref::WorkerBootId::from_raw(1);
  handshake.protocol = ref::ProtocolId::from_valid("ref-wire");
  (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(1));
  (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(2));
  handshake.required_minimum = ref::ProtocolGeneration::from_raw(1);
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(1));
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(2));
  handshake.committed_schema = ref::SchemaGeneration::from_raw(1);
  handshake.capability_generation = ref::CapabilityGeneration::from_raw(1);
  handshake.declared_role = ref::NoteText::from_valid("installed consumer");

  ref::HandshakeOutcome outcome;
  const ref::Status connected = client.connect(handshake, outcome);
  if (connected.is_failure()) {
    std::printf("handshake failed: %s\n", connected.to_string().c_str());
    return 1;
  }
  std::printf("handshake ok: protocol=%llu schema=%llu operation=%s coordinator_epoch=%llu\n",
              static_cast<unsigned long long>(outcome.negotiated_protocol.raw()),
              static_cast<unsigned long long>(outcome.negotiated_schema.raw()),
              ref::to_string(outcome.operation),
              static_cast<unsigned long long>(outcome.coordinator_epoch.raw()));

  ref::WireMessage query;
  (void)query.set_text(ref::FieldId::from_valid("query"), "epochs");
  ref::Frame response;
  ref::WireMessage body;
  const ref::Status queried = client.request(ref::MessageType::QueryState, query, response, body);
  if (queried.is_failure()) {
    std::printf("query failed: %s\n", queried.to_string().c_str());
    return 1;
  }
  const auto json = body.document(ref::FieldId::from_valid("json"));
  std::printf("query result: %s\n", json.has_value() ? std::string(*json).c_str() : "(none)");

  coordinator.stop();
  std::printf("consumer completed successfully\n");
  return 0;
}
