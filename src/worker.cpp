#include "ref/worker.hpp"

#include <cstdlib>

namespace ref {
namespace {

FieldId fid(const char* name) { return FieldId::from_valid(name); }

std::string encode_features(const std::vector<FeatureSupport>& features) {
  std::string out;
  for (const auto& feature : features) {
    if (!out.empty()) out += ',';
    out += feature.feature.str();
    out += ':';
    out += std::to_string(feature.generation.raw());
    out += ':';
    if (feature.can_publish) out += 'p';
    if (feature.can_consume) out += 'c';
  }
  return out;
}

std::string encode_migrations(const std::vector<MigrationCapability>& migrations) {
  std::string out;
  for (const auto& migration : migrations) {
    if (!out.empty()) out += ',';
    out += migration.id.str();
    out += ':';
    out += std::to_string(migration.source.raw());
    out += ':';
    out += std::to_string(migration.target.raw());
  }
  return out;
}

ClientConfig make_client_config(const WorkerConfig& config) {
  ClientConfig client_config;
  client_config.host = config.host;
  client_config.port = config.port;
  client_config.role = config.role.empty() ? NoteText::from_valid("runtime-worker") : config.role;
  return client_config;
}

}  // namespace

RuntimeWorker::RuntimeWorker(WorkerConfig config)
    : config_(std::move(config)), client_(make_client_config(config_)) {}

RuntimeWorker::~RuntimeWorker() { client_.close(); }

ProtocolHandshake RuntimeWorker::make_handshake() const {
  ProtocolHandshake handshake;
  handshake.component = config_.component;
  handshake.version = config_.version;
  handshake.runtime_generation = config_.generation;
  handshake.worker = config_.worker;
  handshake.boot = config_.boot;
  handshake.protocol = config_.protocol;
  handshake.supported_protocols = config_.supported_protocols;
  handshake.required_minimum = config_.supported_protocols.lowest();
  handshake.supported_schemas = config_.supported_schemas;
  handshake.committed_schema = config_.committed_schema.is_set() ? config_.committed_schema
                                                                 : config_.supported_schemas.lowest();
  handshake.capability_generation = config_.capability_generation;
  handshake.features = config_.features;
  handshake.declared_role = config_.role.empty() ? NoteText::from_valid("runtime-worker") : config_.role;
  return handshake;
}

Status RuntimeWorker::read_state(StateFile& file) const {
  if (config_.state_path.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "worker has no state path configured");
  }
  return read_state_file(config_.state_path, file);
}

Status RuntimeWorker::initialize_state(std::string_view owner, std::uint64_t mutation_counter) {
  if (config_.state_path.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "worker has no state path configured");
  }
  if (file_exists(config_.state_path)) return Status::ok();
  StateObject object;
  Status status = object.set_text(FieldId::from_valid("owner"), owner);
  if (status.is_failure()) return status;
  status = object.set_u64(FieldId::from_valid("mutation_counter"), mutation_counter);
  if (status.is_failure()) return status;
  status = object.set_text(FieldId::from_valid("legacy_token"), std::string("token-") + owner.data());
  if (status.is_failure()) return status;
  StateFile file;
  file.header.format = kStateFormatGenerationV1;
  file.header.schema = SchemaId::from_valid("component-state");
  file.header.generation = SchemaGeneration::from_raw(1);
  file.header.migration = MigrationGeneration::unset();
  file.header.epoch = EvolutionEpoch::from_raw(1);
  file.header.policy = PolicyGeneration::unset();
  file.payload = object.encode();
  return write_state_file(config_.state_path, file);
}

Status RuntimeWorker::connect_and_register(HandshakeOutcome& outcome) {
  const Status connected = client_.connect(make_handshake(), outcome);
  if (connected.is_failure()) return connected;
  handshake_ = outcome;

  WireMessage registration;
  (void)registration.set_ident(fid("component"), config_.component);
  (void)registration.set_text(fid("version"), config_.version.number.to_string());
  (void)registration.set_text(fid("artifact"), config_.version.artifact.build.view());
  (void)registration.set_generation(fid("runtime_generation"), config_.generation);
  (void)registration.set_text(fid("supported_protocols"), encode_generation_set(config_.supported_protocols));
  (void)registration.set_text(fid("readable_protocols"), encode_generation_set(config_.supported_protocols));
  (void)registration.set_text(fid("writable_protocols"), encode_generation_set(config_.supported_protocols));
  (void)registration.set_generation(fid("min_safety_protocol"), config_.supported_protocols.lowest());
  (void)registration.set_text(fid("supported_schemas"), encode_generation_set(config_.supported_schemas));
  (void)registration.set_text(fid("readable_schemas"), encode_generation_set(config_.supported_schemas));
  (void)registration.set_text(fid("writable_schemas"), encode_generation_set(config_.supported_schemas));
  (void)registration.set_generation(fid("capability_generation"), config_.capability_generation);
  (void)registration.set_text(fid("lifecycle"), to_string(LifecycleState::Registered));
  (void)registration.set_text(fid("provenance"), to_string(EvidenceClass::Real));
  if (!config_.features.empty()) {
    (void)registration.set_text(fid("features"), encode_features(config_.features));
  }
  if (!config_.migrations.empty()) {
    (void)registration.set_text(fid("migrations"), encode_migrations(config_.migrations));
  }
  Frame response;
  WireMessage body;
  const Status registered = client_.request(MessageType::RegisterComponent, registration, response, body);
  if (registered.is_failure()) return registered;
  const auto status_text = body.text(fid("status"));
  if (!status_text.has_value() || *status_text != "OK") {
    const auto detail = body.text(fid("detail"));
    const auto code = status_text.has_value() ? parse_error_code(*status_text) : std::nullopt;
    std::string message = "runtime registration was refused: ";
    if (status_text.has_value()) message += std::string(*status_text);
    if (detail.has_value()) {
      message += ": ";
      message += std::string(*detail);
    }
    return Status::failure(code.has_value() ? *code : ErrorCode::Conflict, message);
  }
  registered_ = true;
  if (config_.reconcile_on_start) {
    // A real runtime reports what it observes before it is trusted again.
    (void)report_reconciliation();
  }
  return Status::ok();
}

Status RuntimeWorker::report_reconciliation() {
  StateFile file;
  const Status read = read_state(file);
  WireMessage report;
  (void)report.set_ident(fid("component"), config_.component);
  (void)report.set_generation(fid("boot"), config_.boot);
  std::string json = "{\"state_generation\": ";
  json += file.header.generation.is_set() ? std::to_string(file.header.generation.raw()) : "0";
  json += ", \"observed\": \"";
  json += read.is_ok() ? "STATE_DECODED" : "STATE_UNREADABLE";
  json += "\"}";
  (void)report.set_document(fid("json"), json);
  if (read.is_ok()) {
    (void)report.set_generation(fid("state_generation"), file.header.generation);
  }
  (void)report.set_generation(fid("boot"), config_.boot);
  Frame response;
  WireMessage body;
  const Status sent = client_.request(MessageType::ReconcileReport, report, response, body);
  if (sent.is_failure()) return sent;
  const auto status_text = body.text(fid("status"));
  if (!status_text.has_value() || *status_text != "OK") return Status::ok();  // informational
  return Status::ok();
}

Status RuntimeWorker::send_heartbeat() {
  WireMessage heartbeat;
  (void)heartbeat.set_ident(fid("worker"), config_.worker);
  (void)heartbeat.set_generation(fid("boot"), config_.boot);
  (void)heartbeat.set_generation(fid("capability_generation"), config_.capability_generation);
  (void)heartbeat.set_generation(fid("committed_schema"), observed_schema());
  Frame response;
  WireMessage body;
  const Status sent = client_.request(MessageType::Heartbeat, heartbeat, response, body);
  if (sent.is_failure()) return sent;
  const auto status_text = body.text(fid("status"));
  if (!status_text.has_value() || *status_text != "OK") {
    return Status::failure(ErrorCode::StaleBoot, "heartbeat was refused by the coordinator");
  }
  return Status::ok();
}

SchemaGeneration RuntimeWorker::observed_schema() const {
  StateFile file;
  const Status read = read_state(file);
  return read.is_ok() ? file.header.generation : SchemaGeneration::unset();
}

IntegrityDigest RuntimeWorker::observed_state_digest() const {
  StateFile file;
  if (read_state(file).is_failure()) return IntegrityDigest{};
  return file.header.payload_digest;
}

std::uint64_t RuntimeWorker::observed_counter() const {
  StateFile file;
  if (read_state(file).is_failure()) return 0;
  StateObject object;
  if (StateObject::decode(file.payload, object).is_failure()) return 0;
  const StateValue* value = object.find(FieldId::from_valid("mutation_counter"));
  return value == nullptr ? 0 : value->number;
}

Status RuntimeWorker::apply_mutation(std::uint64_t amount) {
  if (!permits_mutation(client_.operation_class())) {
    ++stats_.mutations_rejected;
    return Status::failure(ErrorCode::Unauthorized,
                           "negotiated operation class does not permit mutating this runtime's state");
  }
  StateFile file;
  const Status read = read_state(file);
  if (read.is_failure()) return read;
  StateObject object;
  const Status decoded = StateObject::decode(file.payload, object);
  if (decoded.is_failure()) return decoded;
  const StateValue* current = object.find(FieldId::from_valid("mutation_counter"));
  const std::uint64_t next = (current == nullptr ? 0 : current->number) + amount;
  const Status set = object.set_u64(FieldId::from_valid("mutation_counter"), next);
  if (set.is_failure()) return set;
  file.payload = object.encode();
  const Status written = write_state_file(config_.state_path, file);
  if (written.is_failure()) return written;
  ++stats_.mutations_applied;
  return Status::ok();
}

Status RuntimeWorker::send_completion(const Frame& request, const EvolutionPlanId& plan,
                                      const RolloutStageId& stage_id, StageGeneration stage_generation,
                                      MigrationOutcome outcome, IntegrityDigest digest, bool drain) {
  WireMessage completion;
  (void)completion.set_ident(fid("plan"), plan);
  (void)completion.set_ident(fid("stage_id"), stage_id);
  (void)completion.set_generation(fid("stage_generation"), stage_generation);
  (void)completion.set_text(fid("migration_outcome"), to_string(outcome));
  (void)completion.set_text(fid("state_digest"), digest.to_hex());
  (void)completion.set_ident(fid("worker"), config_.worker);
  (void)completion.set_generation(fid("boot"), config_.boot);
  if (drain) (void)completion.set_bool(fid("drain"), true);

  Frame response;
  WireMessage body;
  const Status sent = client_.request(MessageType::PublishCompletion, completion, response, body);
  (void)request;
  if (sent.is_failure()) return sent;
  const auto status_text = body.text(fid("status"));
  if (!status_text.has_value() || *status_text != "OK") {
    return Status::failure(ErrorCode::Conflict, "coordinator rejected the completion report");
  }
  return Status::ok();
}

Status RuntimeWorker::handle_drain(const Frame& frame, const WireMessage& message) {
  drained_ = true;
  ++stats_.drains_completed;
  // The drain completion echoes the plan binding the coordinator announced, so
  // the coordinator can bind the completion to the stage it belongs to.
  const EvolutionPlanId plan = message.ident<EvolutionPlanIdTag, 63>(fid("plan"));
  const StageGeneration stage_generation = message.generation<StageGenerationTag>(fid("stage_generation"));
  (void)frame;
  return send_completion(frame, plan, RolloutStageId::from_valid("drain"), stage_generation,
                         MigrationOutcome::NotRequired, observed_state_digest(), true);
}

Status RuntimeWorker::handle_migration(const Frame& frame, const WireMessage& message) {
  const EvolutionPlanId plan = message.ident<EvolutionPlanIdTag, 63>(fid("plan"));
  const MigrationId migration = message.ident<MigrationIdTag, 63>(fid("migration"));
  const MigrationGeneration migration_generation =
      message.generation<MigrationGenerationTag>(fid("migration_generation"));
  const SchemaGeneration source = message.generation<SchemaGenerationTag>(fid("source"));
  const SchemaGeneration target = message.generation<SchemaGenerationTag>(fid("target"));
  const RolloutStageId stage_id = RolloutStageId::from_valid("migration");
  const StageGeneration stage_generation = message.generation<StageGenerationTag>(fid("stage_generation"));

  // Refresh authority before mutating: the coordinator may have advanced the
  // rollout since this session was established, and a worker never grants
  // itself authority it has not been given.
  (void)send_heartbeat();
  if (!permits_mutation(client_.operation_class())) {
    // A canary or drained runtime never mutates authoritative state.
    ++stats_.migrations_blocked;
    (void)send_completion(frame, plan, stage_id, stage_generation, MigrationOutcome::Blocked,
                          IntegrityDigest{}, false);
    return Status::failure(ErrorCode::Unauthorized,
                           "migration refused: the negotiated operation class does not permit mutation");
  }

  SchemaRegistry schemas;
  const Status installed = install_builtin_state_schemas(schemas);
  if (installed.is_failure()) return installed;
  StateMigrator migrator(&schemas);

  MigrationContract contract;
  contract.id = migration;
  contract.generation = migration_generation.is_set() ? migration_generation : MigrationGeneration::first();
  contract.schema = SchemaId::from_valid("component-state");
  contract.source = source;
  contract.target = target;
  contract.runtime_generation = config_.generation;
  contract.epoch = frame.header.evolution_epoch.is_set() ? frame.header.evolution_epoch
                                                         : EvolutionEpoch::from_raw(1);
  contract.policy = PolicyGeneration::unset();
  contract.plan = plan;
  contract.irreversible_acknowledged = true;
  contract.preserve_rollback_metadata = true;

  const MigrationResult result = migrator.migrate(config_.state_path, contract);
  if (result.status.is_failure() && result.outcome != MigrationOutcome::Irreversible &&
      result.outcome != MigrationOutcome::RollbackAvailable) {
    ++stats_.migrations_blocked;
    (void)send_completion(frame, plan, stage_id, stage_generation, result.outcome, result.target_digest, false);
    return result.status;
  }
  ++stats_.migrations_committed;
  if (config_.crash_after_migration_commit) {
    // Fault injection for the ambiguous-completion proof: the state is
    // committed, the acknowledgement never leaves the process.
    std::_Exit(97);
  }
  return send_completion(frame, plan, stage_id, stage_generation, result.outcome, result.target_digest, false);
}

Status RuntimeWorker::handle_rollback(const Frame& frame, const WireMessage& message) {
  const EvolutionPlanId plan = message.ident<EvolutionPlanIdTag, 63>(fid("plan"));
  const SchemaGeneration target = message.generation<SchemaGenerationTag>(fid("target_schema"));
  SchemaRegistry schemas;
  const Status installed = install_builtin_state_schemas(schemas);
  if (installed.is_failure()) return installed;
  StateMigrator migrator(&schemas);
  StateFile file;
  const Status read = read_state(file);
  if (read.is_failure()) return read;
  MigrationContract contract;
  contract.id = MigrationId::from_valid("rollback");
  contract.generation = MigrationGeneration::first();
  contract.schema = SchemaId::from_valid("component-state");
  contract.source = file.header.generation;
  contract.target = target;
  contract.runtime_generation = config_.generation;
  contract.epoch = frame.header.evolution_epoch.is_set() ? frame.header.evolution_epoch
                                                         : EvolutionEpoch::from_raw(1);
  contract.plan = plan;
  const MigrationResult result = migrator.reverse(config_.state_path, contract);
  if (result.status.is_ok()) ++stats_.rollbacks_applied;
  (void)send_completion(frame, plan, RolloutStageId::from_valid("rollback"), StageGeneration::from_raw(0),
                        result.outcome, result.target_digest, false);
  return result.status;
}

Status RuntimeWorker::handle_state_transform(const Frame& frame, const WireMessage& message) {
  if (client_.negotiated_protocol() < ProtocolGeneration::from_raw(2)) {
    ++stats_.frames_rejected;
    return Status::failure(ErrorCode::Unauthorized,
                           "state transformation requires the protocol-2 negotiation");
  }
  return handle_migration(frame, message);
}

Status RuntimeWorker::serve_one() {
  Frame frame;
  const Status received = recv_frame(client_.connection(), frame);
  if (received.is_failure()) return received;
  ++stats_.frames_received;
  const Status envelope = validate_frame_envelope(frame);
  if (envelope.is_failure()) {
    ++stats_.frames_rejected;
    return envelope;
  }
  // A coordinator frame must carry the epochs the session agreed on.
  if (frame.header.boot != config_.boot) {
    ++stats_.frames_rejected;
    return Status::failure(ErrorCode::StaleBoot, "frame boot identity does not match this process");
  }
  if (frame.header.protocol_generation.is_set() &&
      frame.header.protocol_generation > client_.negotiated_protocol()) {
    ++stats_.frames_rejected;
    return Status::failure(ErrorCode::StaleGeneration,
                           "frame protocol generation exceeds the negotiated generation");
  }
  WireMessage message;
  const Status decoded = WireMessage::decode(frame.payload, message);
  if (decoded.is_failure()) {
    ++stats_.frames_rejected;
    return decoded;
  }
  switch (frame.header.type) {
    case MessageType::RequestDrain:
      return handle_drain(frame, message);
    case MessageType::RequestMigration:
      return handle_migration(frame, message);
    case MessageType::RequestRollback:
      return handle_rollback(frame, message);
    case MessageType::RequestStateTransform:
      return handle_state_transform(frame, message);
    case MessageType::Error:
    case MessageType::Goodbye:
      return Status::failure(ErrorCode::Retired, "coordinator closed the session");
    default:
      ++stats_.frames_rejected;
      return Status::failure(ErrorCode::Unsupported, "worker does not accept this message type");
  }
}

Status RuntimeWorker::serve() {
  while (!stop_requested_) {
    const Status served = serve_one();
    if (served.is_failure()) return served;
  }
  return Status::ok();
}

}  // namespace ref