#include "ref/client.hpp"

#include "ref/coordinator.hpp"

namespace ref {
namespace {

std::string field_text(const WireMessage& body, const char* name) {
  const auto value = body.text(FieldId::from_valid(name));
  return value.has_value() ? std::string(*value) : std::string();
}

std::uint64_t field_u64(const WireMessage& body, const char* name) {
  const auto value = body.u64(FieldId::from_valid(name));
  return value.has_value() ? *value : 0;
}

}  // namespace

EvolutionClient::EvolutionClient(ClientConfig config)
    : config_(std::move(config)), sequence_(config_.sequence_seed) {}

EvolutionClient::~EvolutionClient() { close(); }

void EvolutionClient::close() noexcept {
  connected_ = false;
  connection_.close();
}

Status EvolutionClient::connect(const ProtocolHandshake& handshake, HandshakeOutcome& outcome) {
  outcome = HandshakeOutcome{};
  SocketRuntime runtime;
  if (!runtime.ok()) return Status::failure(ErrorCode::Internal, runtime.error());
  TcpConnection connection;
  const Status established = connect_tcp(connection, config_.host, config_.port);
  if (established.is_failure()) return established;
  connection.set_no_delay();
  connection_ = std::move(connection);
  connected_ = true;
  Frame hello;
  hello.header.type = MessageType::Hello;
  hello.header.flags = config_.operator_mode ? kFrameFlagOperator : kFrameFlagNone;
  hello.header.runtime_generation = handshake.runtime_generation;
  hello.header.protocol_generation = handshake.supported_protocols.highest();
  hello.header.schema_generation = handshake.supported_schemas.highest();
  hello.header.evolution_epoch = handshake.evolution_epoch;
  hello.header.coordinator_epoch = handshake.coordinator_epoch;
  hello.header.boot = handshake.boot;
  hello.header.sequence = next_sequence();

  WireMessage payload;
  (void)payload.set_ident(FieldId::from_valid("component"), handshake.component);
  (void)payload.set_text(FieldId::from_valid("version"), handshake.version.number.to_string());
  (void)payload.set_text(FieldId::from_valid("artifact"), handshake.version.artifact.build.view());
  (void)payload.set_generation(FieldId::from_valid("runtime_generation"), handshake.runtime_generation);
  (void)payload.set_ident(FieldId::from_valid("worker"), handshake.worker);
  (void)payload.set_generation(FieldId::from_valid("boot"), handshake.boot);
  (void)payload.set_ident(FieldId::from_valid("protocol"), handshake.protocol);
  (void)payload.set_text(FieldId::from_valid("supported_protocols"),
                         encode_generation_set(handshake.supported_protocols));
  (void)payload.set_generation(FieldId::from_valid("required_minimum"), handshake.required_minimum);
  (void)payload.set_text(FieldId::from_valid("supported_schemas"),
                         encode_generation_set(handshake.supported_schemas));
  (void)payload.set_generation(FieldId::from_valid("committed_schema"), handshake.committed_schema);
  (void)payload.set_generation(FieldId::from_valid("capability_generation"), handshake.capability_generation);
  (void)payload.set_generation(FieldId::from_valid("evolution_epoch"), handshake.evolution_epoch);
  (void)payload.set_generation(FieldId::from_valid("coordinator_epoch"), handshake.coordinator_epoch);
  (void)payload.set_text(FieldId::from_valid("declared_role"), handshake.declared_role.view());
  {
    std::string features;
    for (const auto& feature : handshake.features) {
      if (!features.empty()) features += ',';
      features += feature.feature.str();
      features += ':';
      features += std::to_string(feature.generation.raw());
      features += ':';
      if (feature.can_publish) features += 'p';
      if (feature.can_consume) features += 'c';
    }
    if (!features.empty()) (void)payload.set_text(FieldId::from_valid("features"), features);
  }
  const std::string encoded = payload.encode();
  if (encoded.size() > limits::kFrameBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "handshake frame exceeds the frame bound");
  }
  hello.payload = encoded;

  const Status sent = send_frame(connection_, hello);
  if (sent.is_failure()) {
    close();
    return sent;
  }
  Frame response;
  const Status received = recv_frame(connection_, response);
  if (received.is_failure()) {
    close();
    return received;
  }
  WireMessage body;
  const Status decoded = WireMessage::decode(response.payload, body);
  if (decoded.is_failure()) {
    close();
    return decoded;
  }
  if (response.header.type != MessageType::HelloAck) {
    close();
    return Status::failure(ErrorCode::Conflict, "coordinator answered the handshake with an unexpected message");
  }
  outcome = parse_handshake_ack(response, body);
  if (!outcome.is_ok()) {
    close();
    return outcome.status;
  }
  peer_.component = handshake.component;
  peer_.version = handshake.version;
  peer_.worker = handshake.worker;
  peer_.boot = handshake.boot;
  peer_.runtime_generation = handshake.runtime_generation;
  peer_.protocol = handshake.protocol;
  peer_.agreed_protocol = outcome.negotiated_protocol;
  peer_.agreed_schema = outcome.negotiated_schema;
  peer_.operation = outcome.operation;
  peer_.epoch = outcome.epoch;
  peer_.coordinator_epoch = outcome.coordinator_epoch;
  peer_.capability_generation = handshake.capability_generation;
  peer_.registered = outcome.registered;
  peer_.is_operator = config_.operator_mode;
  peer_.authenticated = config_.operator_mode;
  return Status::ok();
}

Status EvolutionClient::request(MessageType type, const WireMessage& payload, Frame& response,
                                WireMessage& body) {
  Frame frame;
  frame.header.type = type;
  frame.header.flags = config_.operator_mode ? kFrameFlagOperator : kFrameFlagNone;
  frame.header.runtime_generation = peer_.runtime_generation;
  frame.header.protocol_generation = peer_.agreed_protocol;
  frame.header.schema_generation = peer_.agreed_schema;
  frame.header.coordinator_epoch = peer_.coordinator_epoch;
  frame.header.evolution_epoch = peer_.epoch;
  frame.header.boot = peer_.boot;
  frame.header.sequence = next_sequence();
  frame.payload = payload.encode();
  if (frame.payload.size() > limits::kFrameBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "request frame exceeds the frame bound");
  }
  return send_raw(frame, response, body);
}

Status EvolutionClient::send_raw(const Frame& frame, Frame& response, WireMessage& body) {
  if (!connected_) return Status::failure(ErrorCode::IoFailure, "client is not connected");
  Frame outbound = frame;
  if (outbound.header.flags == kFrameFlagNone && config_.operator_mode) {
    outbound.header.flags = kFrameFlagOperator;
  }
  const Status sent = send_frame(connection_, outbound);
  if (sent.is_failure()) return sent;
  const Status received = recv_frame(connection_, response);
  if (received.is_failure()) {
    close();
    return received;
  }
  return WireMessage::decode(response.payload, body);
}

HandshakeOutcome parse_handshake_ack(const Frame& response, const WireMessage& body) {
  (void)response;  // the response frame carries the header; the body carries the outcome
  HandshakeOutcome outcome;
  const std::string status = field_text(body, "status");
  const std::string detail = field_text(body, "detail");
  outcome.detail = detail;
  if (status != "OK") {
    const auto code = parse_error_code(status);
    outcome.status = Status::failure(code.has_value() ? *code : ErrorCode::Internal, detail);
    return outcome;
  }
  const std::string json = field_text(body, "json");
  outcome.negotiated_protocol = ProtocolGeneration::from_raw(field_u64(body, "negotiated_protocol"));
  outcome.negotiated_schema = SchemaGeneration::from_raw(field_u64(body, "negotiated_schema"));
  const auto operation = parse_operation_class(field_text(body, "operation_class"));
  outcome.operation = operation.has_value() ? *operation : OperationClass::None;
  outcome.epoch = EvolutionEpoch::from_raw(field_u64(body, "evolution_epoch"));
  outcome.coordinator_epoch = CoordinatorEpoch::from_raw(field_u64(body, "coordinator_epoch"));
  outcome.stage_generation = StageGeneration::from_raw(field_u64(body, "stage_generation"));
  outcome.gate_generation = FeatureGateGeneration::from_raw(field_u64(body, "gate_generation"));
  outcome.matrix_generation = CompatibilityGeneration::from_raw(field_u64(body, "matrix_generation"));
  outcome.authoritative_generation = RuntimeGeneration::from_raw(field_u64(body, "authoritative_generation"));
  outcome.registered = field_u64(body, "registered") != 0;
  outcome.new_writer_enabled = field_u64(body, "new_writer_enabled") != 0;
  if (field_u64(body, "negotiated_protocol") == 0) {
    outcome.status = Status::failure(ErrorCode::Incompatible,
                                     "coordinator acknowledged without a negotiated protocol generation");
    return outcome;
  }
  // The JSON document is carried as text; extract the explanation array markers
  // only when present so the CLI can print it verbatim.
  outcome.explanation = json;
  outcome.status = Status::ok();
  return outcome;
}

}  // namespace ref