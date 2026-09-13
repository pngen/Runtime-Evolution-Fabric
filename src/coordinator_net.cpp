// Runtime Evolution Fabric - coordinator network service.
//
// One acceptor thread, one reader thread per connection, one writer thread per
// connection and exactly one event-processing thread that owns all mutation.
// Frame validation happens on the event thread before any handler runs, so an
// unauthorized, stale or malformed frame is rejected before it can mutate
// anything.
#include <algorithm>

#include "coordinator_internal.hpp"
#include "ref/coordinator.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace ref {
namespace {

constexpr std::size_t kSessionQueueDepth = 256;

bool peer_is_loopback(std::uintptr_t handle) {
#if defined(_WIN32)
  sockaddr_in address{};
  int length = sizeof(address);
  if (getpeername(static_cast<SOCKET>(handle), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return false;
  }
#else
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (getpeername(static_cast<int>(handle), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return false;
  }
#endif
  const std::uint32_t host = ntohl(address.sin_addr.s_addr);
  return (host >> 24) == 127;
}

std::vector<FeatureSupport> decode_features(std::string_view text) {
  std::vector<FeatureSupport> features;
  std::size_t start = 0;
  while (start < text.size() && features.size() < limits::kFeatureGates) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view entry = text.substr(start, end - start);
    const std::size_t first = entry.find(':');
    const std::size_t second = first == std::string_view::npos ? std::string_view::npos : entry.find(':', first + 1);
    if (first != std::string_view::npos) {
      FeatureSupport support;
      support.feature = FeatureGateId::from_valid(entry.substr(0, first));
      bool ok = false;
      if (second != std::string_view::npos) {
        support.generation = FeatureGateGeneration::from_raw(parse_u64(entry.substr(first + 1, second - first - 1), ok));
        const std::string_view flags = entry.substr(second + 1);
        support.can_publish = flags.find('p') != std::string_view::npos;
        support.can_consume = flags.find('c') != std::string_view::npos;
      } else {
        support.generation = FeatureGateGeneration::from_raw(parse_u64(entry.substr(first + 1), ok));
      }
      if (!support.feature.empty()) features.push_back(support);
    }
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return features;
}

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

}  // namespace

struct EvolutionCoordinator::Session {
  SessionId id{};
  TcpConnection connection{};
  bool loopback{false};
  std::atomic<bool> closed{false};
  bool handshaked{false};
  PeerIdentity peer{};
  std::uint64_t last_sequence{0};
  std::unique_ptr<BoundedQueue<Frame>> outgoing{};
  // Both worker threads hold a shared_ptr to this session, so the object always
  // outlives them; it is destroyed only by the reaper on the accept or event
  // thread, never on one of its own threads. Destroying a session on its own
  // reader thread would destroy a joinable std::thread and terminate the
  // process, so the design forbids it.
  std::atomic<bool> reader_done{false};
  std::atomic<bool> writer_done{false};
  std::atomic<bool> reap_ready{false};
  std::thread reader{};
  std::thread writer{};
};

struct EvolutionCoordinator::Event {
  std::shared_ptr<Session> session{};
  Frame frame{};
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
EvolutionCoordinator::EvolutionCoordinator(CoordinatorConfig config) : config_(std::move(config)) {
  events_ = std::make_unique<BoundedQueue<Event>>(config_.event_queue_depth);
}

EvolutionCoordinator::~EvolutionCoordinator() { stop(); }

Status EvolutionCoordinator::start() {
  if (running_.load()) return Status::failure(ErrorCode::Conflict, "coordinator is already running");
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status loaded = load_durable_locked();
    if (loaded.is_failure()) return loaded;
  }
  Status bootstrap = install_builtin_protocols();
  if (bootstrap.is_failure()) return bootstrap;
  bootstrap = install_builtin_schemas();
  if (bootstrap.is_failure()) return bootstrap;
  bootstrap = install_builtin_features();
  if (bootstrap.is_failure()) return bootstrap;

  const Status opened = listener_.open(config_.bind_host, config_.port);
  if (opened.is_failure()) return opened;
  listener_port_ = listener_.port();
  running_ = true;
  accept_thread_ = std::thread([this] { accept_loop(); });
  event_thread_ = std::thread([this] { event_loop(); });
  return Status::ok();
}

void EvolutionCoordinator::stop() {
  if (!running_.exchange(false)) {
    // Still close the listener in case start() failed halfway.
    listener_.close();
    if (events_ != nullptr) events_->close();
    return;
  }
  listener_.close();
  shutdown_sessions();
  if (events_ != nullptr) events_->close();
  if (accept_thread_.joinable()) accept_thread_.join();
  if (event_thread_.joinable()) event_thread_.join();
  listener_port_ = 0;
}

void EvolutionCoordinator::shutdown_sessions() {
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> guard(connections_mutex_);
    sessions.reserve(sessions_.size());
    for (auto& [id, session] : sessions_) {
      (void)id;
      sessions.push_back(session);
    }
  }
  // Close sockets and queues first so blocked reads and writes return, then
  // join outside every lock.
  for (const auto& session : sessions) {
    session->closed = true;
    session->connection.close();
    session->outgoing->close();
  }
  for (const auto& session : sessions) {
    if (session->reader.joinable()) session->reader.join();
    if (session->writer.joinable()) session->writer.join();
  }
  {
    std::lock_guard<std::mutex> guard(connections_mutex_);
    sessions_.clear();
  }
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    worker_sessions_.clear();
    live_workers_.clear();
  }
}

void EvolutionCoordinator::refresh_worker_authority_locked(const RuntimeComponentId& component) {
  // Authority is re-derived for every live session of the component so a stage
  // change takes effect on the next frame instead of at the next handshake.
  std::lock_guard<std::mutex> guard(connections_mutex_);
  for (auto& [id, session] : sessions_) {
    (void)id;
    if (session == nullptr) continue;
    if (session->peer.component != component) continue;
    if (!session->handshaked) continue;
    const OperationClass operation = derive_operation_class_locked(
        component, session->peer.runtime_generation, session->peer.agreed_protocol);
    session->peer.operation = operation;
    const auto live = live_workers_.find(session->peer.worker);
    if (live != live_workers_.end()) live->second.operation = operation;
  }
}

void EvolutionCoordinator::mark_session_done(const std::shared_ptr<Session>& session, bool reader) {
  if (reader) {
    session->reader_done = true;
  } else {
    session->writer_done = true;
  }
  if (session->reader_done.load() && session->writer_done.load()) {
    bool expected = false;
    if (session->reap_ready.compare_exchange_strong(expected, true)) {
      finished_sessions_.fetch_add(1);
    }
  }
}

void EvolutionCoordinator::reap_sessions() {
  std::vector<std::shared_ptr<Session>> done;
  {
    std::lock_guard<std::mutex> guard(connections_mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if (!it->second->reap_ready.load()) {
        ++it;
        continue;
      }
      done.push_back(it->second);
      it = sessions_.erase(it);
    }
  }
  for (const auto& session : done) {
    // Joining and destroying happen here, outside every lock and never on the
    // session's own threads.
    if (session->reader.joinable()) session->reader.join();
    if (session->writer.joinable()) session->writer.join();
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const auto mapping = worker_sessions_.find(session->peer.worker);
    if (mapping != worker_sessions_.end() && mapping->second == session->id) {
      worker_sessions_.erase(mapping);
      live_workers_.erase(session->peer.worker);
    }
  }
  if (!done.empty()) finished_sessions_.store(0);
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
void EvolutionCoordinator::accept_loop() {
  while (running_.load()) {
    reap_sessions();
    TcpConnection connection;
    const Status accepted = listener_.accept(connection);
    if (!running_.load()) break;
    if (accepted.is_failure()) continue;
    connection.set_no_delay();

    auto session = std::make_shared<Session>();
    session->connection = std::move(connection);
    session->loopback = peer_is_loopback(session->connection.native_handle());
    session->outgoing = std::make_unique<BoundedQueue<Frame>>(kSessionQueueDepth);
    {
      std::lock_guard<std::mutex> guard(connections_mutex_);
      if (sessions_.size() >= limits::kConnections) {
        session->closed = true;
        session->connection.close();
        internal::StateLockGuard state_guard(state_mutex_, state_lock_owner_);
        ++stats_.connections_rejected;
        continue;
      }
      session->id = SessionId::from_valid("s-" + std::to_string(++session_counter_));
      sessions_[session->id] = session;
      internal::StateLockGuard state_guard(state_mutex_, state_lock_owner_);
      ++stats_.connections_accepted;
    }
    session->reader = std::thread([this, session] {
      session_reader(session);
      mark_session_done(session, true);
    });
    session->writer = std::thread([this, session] {
      while (true) {
        auto item = session->outgoing->wait_pop();
        if (!item.has_value()) break;
        const Status sent = send_frame(session->connection, item->second);
        if (sent.is_failure()) {
          session->closed = true;
          session->connection.close();
          session->outgoing->close();
          break;
        }
      }
      mark_session_done(session, false);
    });
  }
}

void EvolutionCoordinator::session_reader(std::shared_ptr<Session> session) {
  while (running_.load() && !session->closed.load()) {
    Frame frame;
    const Status received = recv_frame(session->connection, frame);
    if (received.is_failure()) break;
    const auto sequence = events_->offer(Event{session, std::move(frame)});
    if (!sequence.has_value()) {
      // Explicit backpressure: the frame is refused, not silently dropped.
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.queue_rejections;
      WireMessage body;
      (void)body.set_text(internal::field_id("status"), "QUEUE_FULL");
      (void)body.set_text(internal::field_id("detail"),
                          "coordinator event queue is full; frame rejected without mutation");
      Frame response;
      response.header.type = MessageType::Error;
      response.header.flags = kFrameFlagResponse | kFrameFlagFatal;
      response.header.coordinator_epoch = durable_.coordinator_epoch;
      response.payload = body.encode();
      (void)session->outgoing->offer(response);
    }
  }
  session->closed = true;
  session->connection.close();
  session->outgoing->close();
}

void EvolutionCoordinator::event_loop() {
  while (running_.load()) {
    auto item = events_->wait_pop();
    if (!item.has_value()) break;
    Event event = std::move(item->second);
    handle_event(std::move(event));
    {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.events_processed;
    }
    // Sessions whose threads have both ended are reaped here, on a thread that
    // does not belong to any session.
    if (finished_sessions_.load() > 0) reap_sessions();
  }
}

void EvolutionCoordinator::handle_event(Event event) {
  if (event.session == nullptr) return;
  handle_frame(event.session, event.frame);
}

// ---------------------------------------------------------------------------
// Response plumbing
// ---------------------------------------------------------------------------
void EvolutionCoordinator::respond(const std::shared_ptr<Session>& session, const Frame& request,
                                   MessageType type, const CommandResult& result,
                                   std::uint32_t extra_flags) {
  WireMessage body = result.fields;
  (void)body.set_text(internal::field_id("status"), to_string(result.status.code()));
  (void)body.set_text(internal::field_id("detail"), result.status.detail());
  if (!result.json.empty()) (void)body.set_document(internal::field_id("json"), result.json);
  Frame response;
  response.header.type = type;
  response.header.flags = kFrameFlagResponse | extra_flags;
  response.header.runtime_generation = request.header.runtime_generation;
  response.header.protocol_generation = request.header.protocol_generation;
  response.header.schema_generation = request.header.schema_generation;
  response.header.coordinator_epoch = durable_.coordinator_epoch;
  response.header.evolution_epoch = request.header.evolution_epoch;
  response.header.boot = request.header.boot;
  response.header.sequence = ++frame_counter_;
  const std::string encoded = body.encode();
  if (encoded.size() > limits::kFrameBytes) return;
  response.payload = encoded;
  (void)session->outgoing->offer(std::move(response));
}

Status EvolutionCoordinator::send_to_worker_locked(const WorkerId& worker, MessageType type,
                                                   const WireMessage& payload, const Frame& template_frame) {
  std::shared_ptr<Session> session;
  {
    const auto mapping = worker_sessions_.find(worker);
    if (mapping == worker_sessions_.end()) {
      return Status::failure(ErrorCode::NotFound, "worker has no live session");
    }
    std::lock_guard<std::mutex> guard(connections_mutex_);
    const auto it = sessions_.find(mapping->second);
    if (it == sessions_.end()) return Status::failure(ErrorCode::NotFound, "worker session is gone");
    session = it->second;
  }
  const std::string encoded = payload.encode();
  if (encoded.size() > limits::kFrameBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "outbound frame exceeds the frame bound");
  }
  Frame frame = template_frame;
  frame.header.type = type;
  frame.header.magic = kWireMagic;
  frame.header.transport_version = kWireTransportVersion;
  frame.header.flags = kFrameFlagNone;
  frame.header.payload_length = static_cast<std::uint32_t>(encoded.size());
  frame.header.coordinator_epoch = durable_.coordinator_epoch;
  frame.header.sequence = ++frame_counter_;
  frame.payload = encoded;
  if (!session->outgoing->offer(std::move(frame)).has_value()) {
    return Status::failure(ErrorCode::QueueFull, "worker session queue is full; action not dispatched");
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Frame semantics
// ---------------------------------------------------------------------------
Status EvolutionCoordinator::validate_frame_semantics_locked(const Session& session, const Frame& frame,
                                                             WireMessage& message) const {
  const PeerIdentity& peer = session.peer;
  if (!session.handshaked) {
    return Status::failure(ErrorCode::Unauthorized, "session has not completed a handshake");
  }
  if (frame.header.runtime_generation != peer.runtime_generation) {
    return Status::failure(ErrorCode::StaleGeneration,
                           "frame runtime generation does not match the negotiated session");
  }
  if (frame.header.boot != peer.boot) {
    return Status::failure(ErrorCode::StaleBoot, "frame boot identity does not match the session");
  }
  if (durable_.is_retired(peer.component, peer.runtime_generation)) {
    return Status::failure(ErrorCode::RetiredGeneration, "runtime generation is retired");
  }
  if (durable_.is_boot_fenced(peer.worker, frame.header.boot)) {
    return Status::failure(ErrorCode::StaleBoot, "boot identity has been fenced");
  }
  const auto live = live_workers_.find(peer.worker);
  if (live == live_workers_.end()) {
    return Status::failure(ErrorCode::StaleBoot, "worker is not current; re-registration is required");
  }
  if (live->second.fenced) {
    return Status::failure(ErrorCode::StaleBoot, "worker has been fenced");
  }
  if (frame.header.coordinator_epoch != durable_.coordinator_epoch) {
    return Status::failure(ErrorCode::StaleEpoch, "frame carries a stale coordinator epoch");
  }
  const EvolutionEpoch required_epoch = epoch_for_locked(peer.component);
  if (frame.header.evolution_epoch != required_epoch) {
    return Status::failure(ErrorCode::StaleEpoch, "frame carries a stale evolution epoch");
  }
  if (frame.header.protocol_generation != peer.agreed_protocol) {
    return Status::failure(ErrorCode::StaleGeneration,
                           "frame protocol generation is not the negotiated generation");
  }
  if (frame.header.schema_generation != peer.agreed_schema) {
    return Status::failure(ErrorCode::StaleGeneration,
                           "frame schema generation is not the negotiated generation");
  }
  const ProtocolDescriptor* descriptor = durable_.protocols.find(peer.protocol, peer.agreed_protocol);
  if (descriptor == nullptr) {
    return Status::failure(ErrorCode::Unsupported, "negotiated protocol generation is not registered");
  }
  const MessageTypeId type_id = MessageTypeId::from_valid(to_string(frame.header.type));
  if (!descriptor->supports_message(type_id)) {
    return Status::failure(ErrorCode::Unsupported,
                           "message type is not available at the negotiated protocol generation");
  }
  const WireMessageSpec* spec = wire_message_spec(frame.header.type);
  if (spec == nullptr) {
    return Status::failure(ErrorCode::Unsupported, "message type has no declared field contract");
  }
  if (spec->operator_only && !peer.is_operator) {
    return Status::failure(ErrorCode::Unauthorized, "message type requires operator authority");
  }
  const Status decoded = WireMessage::decode(frame.payload, message);
  if (decoded.is_failure()) return decoded;
  for (const auto& field : message.fields()) {
    const MessageTypeDescriptor* declared = descriptor->find_message(type_id);
    if (declared == nullptr) {
      return Status::failure(ErrorCode::Unsupported, "message type is not declared by the protocol");
    }
    bool known = false;
    for (const auto& declared_field : declared->fields) {
      if (declared_field.id == field.name) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Status::failure(ErrorCode::Unsupported, "message carries a field the protocol does not declare");
    }
  }
  std::vector<const char*> required(spec->required.begin(), spec->required.end());
  const Status required_status = require_fields(message, required);
  if (required_status.is_failure()) return required_status;
  if (message_is_mutating(frame.header.type) && !permits_mutation(peer.operation)) {
    // Bootstrap exceptions: a peer that is not yet registered has no operation
    // class, so registration and capability publication must be reachable
    // without mutation authority. Nothing else is.
    const bool bootstrap = frame.header.type == MessageType::RegisterComponent ||
                           frame.header.type == MessageType::PublishCapability;
    if (!bootstrap) {
      return Status::failure(ErrorCode::Unauthorized,
                             "negotiated operation class does not permit mutating messages");
    }
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------
void EvolutionCoordinator::handle_frame(const std::shared_ptr<Session>& session, const Frame& frame) {
  const Status envelope = validate_frame_envelope(frame);
  if (envelope.is_failure()) {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    ++stats_.frames_rejected;
    return;
  }
  if (frame.header.type == MessageType::Hello) {
    WireMessage message;
    const Status decoded = WireMessage::decode(frame.payload, message);
    if (decoded.is_failure()) {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.frames_rejected;
      respond(session, frame, MessageType::Error, internal::failure_result(decoded), kFrameFlagFatal);
      return;
    }
    ProtocolHandshake handshake;
    handshake.component = message.ident<RuntimeComponentIdTag, 63>(internal::field_id("component"));
    const auto version_text = message.text(internal::field_id("version"));
    if (version_text.has_value()) {
      const auto parsed = VersionNumber::parse(*version_text);
      if (parsed.has_value()) handshake.version.number = *parsed;
    }
    const auto artifact = message.text(internal::field_id("artifact"));
    if (artifact.has_value()) handshake.version.artifact.build = BuildId::from_valid(*artifact);
    handshake.runtime_generation = message.generation<RuntimeGenerationTag>(internal::field_id("runtime_generation"));
    handshake.worker = message.ident<WorkerIdTag, 63>(internal::field_id("worker"));
    handshake.boot = message.generation<WorkerBootIdTag>(internal::field_id("boot"));
    handshake.protocol = message.ident<ProtocolIdTag, 63>(internal::field_id("protocol"));
    const auto protocols = message.text(internal::field_id("supported_protocols"));
    if (protocols.has_value()) {
      (void)decode_generation_set(*protocols, handshake.supported_protocols);
    }
    handshake.required_minimum =
        message.generation<ProtocolGenerationTag>(internal::field_id("required_minimum"));
    const auto schemas = message.text(internal::field_id("supported_schemas"));
    if (schemas.has_value()) (void)decode_generation_set(*schemas, handshake.supported_schemas);
    handshake.committed_schema =
        message.generation<SchemaGenerationTag>(internal::field_id("committed_schema"));
    handshake.capability_generation =
        message.generation<CapabilityGenerationTag>(internal::field_id("capability_generation"));
    handshake.evolution_epoch = message.generation<EvolutionEpochTag>(internal::field_id("evolution_epoch"));
    handshake.coordinator_epoch = message.generation<CoordinatorEpochTag>(internal::field_id("coordinator_epoch"));
    const auto features = message.text(internal::field_id("features"));
    if (features.has_value()) handshake.features = decode_features(*features);
    const auto role = message.text(internal::field_id("declared_role"));
    if (role.has_value()) handshake.declared_role = NoteText::from_valid(*role);

    PeerIdentity peer;
    peer.session = session->id;
    peer.is_operator = session->loopback && ((frame.header.flags & kFrameFlagOperator) != 0);
    peer.authenticated = peer.is_operator;
    const CommandResult result = command_hello(handshake, peer);
    if (result.is_ok()) {
      session->peer = peer;
      session->handshaked = true;
      session->last_sequence = frame.header.sequence;
    } else {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.frames_rejected;
    }
    respond(session, frame, MessageType::HelloAck, result);
    return;
  }

  WireMessage message;
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    const Status valid = validate_frame_semantics_locked(*session, frame, message);
    if (valid.is_failure()) {
      ++stats_.frames_rejected;
      respond(session, frame, MessageType::Error, internal::failure_result(valid));
      return;
    }
    if (frame.header.sequence <= session->last_sequence) {
      ++stats_.frames_rejected;
      respond(session, frame, MessageType::Error,
              internal::failure_result(Status::failure(
                  ErrorCode::Conflict, "frame sequence is not ahead of the session watermark")));
      return;
    }
    session->last_sequence = frame.header.sequence;
    auto worker = live_workers_.find(session->peer.worker);
    if (worker != live_workers_.end()) {
      ++worker->second.frames;
      worker->second.evidence = durable_.evidence_generation;
    }
  }

  const PeerIdentity peer = session->peer;
  const FieldId f_component = internal::field_id("component");
  const FieldId f_generation = internal::field_id("runtime_generation");
  const FieldId f_plan = internal::field_id("plan");
  const FieldId f_stage = internal::field_id("stage");
  const FieldId f_stage_generation = internal::field_id("stage_generation");
  const FieldId f_stage_id = internal::field_id("stage_id");
  const FieldId f_query = internal::field_id("query");
  const FieldId f_worker = internal::field_id("worker");
  const FieldId f_boot = internal::field_id("boot");
  const FieldId f_reason = internal::field_id("reason");
  const FieldId f_json = internal::field_id("json");
  const FieldId f_migration_outcome = internal::field_id("migration_outcome");
  const FieldId f_state_digest = internal::field_id("state_digest");
  const FieldId f_target_schema = internal::field_id("target_schema");

  CommandResult result;
  MessageType response_type = MessageType::Error;
  switch (frame.header.type) {
    case MessageType::RegisterComponent: {
      RuntimeComponentVersion version;
      version.component = message.ident<RuntimeComponentIdTag, 63>(f_component);
      const auto version_text = message.text(internal::field_id("version"));
      if (version_text.has_value()) {
        const auto parsed = VersionNumber::parse(*version_text);
        if (parsed.has_value()) version.version.number = *parsed;
      }
      const auto artifact = message.text(internal::field_id("artifact"));
      if (artifact.has_value()) version.version.artifact.build = BuildId::from_valid(*artifact);
      version.generation = message.generation<RuntimeGenerationTag>(f_generation);
      const auto protocols = message.text(internal::field_id("supported_protocols"));
      if (protocols.has_value()) (void)decode_generation_set(*protocols, version.protocols.supported);
      const auto readable = message.text(internal::field_id("readable_protocols"));
      if (readable.has_value()) {
        (void)decode_generation_set(*readable, version.protocols.readable);
      } else {
        version.protocols.readable = version.protocols.supported;
      }
      const auto writable = message.text(internal::field_id("writable_protocols"));
      if (writable.has_value()) {
        (void)decode_generation_set(*writable, version.protocols.writable);
      } else {
        version.protocols.writable = version.protocols.supported;
      }
      version.protocols.minimum_safety =
          message.generation<ProtocolGenerationTag>(internal::field_id("min_safety_protocol"));
      if (!version.protocols.minimum_safety.is_set()) {
        version.protocols.minimum_safety = ProtocolGeneration::from_raw(1);
      }
      const auto schemas = message.text(internal::field_id("supported_schemas"));
      if (schemas.has_value()) (void)decode_generation_set(*schemas, version.schemas.supported);
      const auto schema_readable = message.text(internal::field_id("readable_schemas"));
      if (schema_readable.has_value()) {
        (void)decode_generation_set(*schema_readable, version.schemas.readable);
      } else {
        version.schemas.readable = version.schemas.supported;
      }
      const auto schema_writable = message.text(internal::field_id("writable_schemas"));
      if (schema_writable.has_value()) {
        (void)decode_generation_set(*schema_writable, version.schemas.writable);
      } else {
        version.schemas.writable = version.schemas.supported;
      }
      // State *format* generations describe the durable container, not the
      // logical schema: they are declared explicitly and default to the
      // container generation this build understands.
      const auto formats_readable = message.text(internal::field_id("readable_formats"));
      if (formats_readable.has_value()) {
        (void)decode_generation_set(*formats_readable, version.schemas.readable_formats);
      } else {
        (void)version.schemas.readable_formats.add(kStateFormatGenerationV1);
      }
      const auto formats_writable = message.text(internal::field_id("writable_formats"));
      if (formats_writable.has_value()) {
        (void)decode_generation_set(*formats_writable, version.schemas.writable_formats);
      } else {
        (void)version.schemas.writable_formats.add(kStateFormatGenerationV1);
      }
      version.capability_generation =
          message.generation<CapabilityGenerationTag>(internal::field_id("capability_generation"));
      if (!version.capability_generation.is_set()) version.capability_generation = CapabilityGeneration::first();
      const auto lifecycle = message.text(internal::field_id("lifecycle"));
      if (lifecycle.has_value()) {
        const auto parsed = parse_lifecycle(*lifecycle);
        if (parsed.has_value()) version.lifecycle = *parsed;
      }
      const auto provenance = message.text(internal::field_id("provenance"));
      if (provenance.has_value()) {
        const auto parsed = parse_evidence_class(*provenance);
        if (parsed.has_value()) version.provenance = *parsed;
      } else {
        version.provenance = EvidenceClass::Real;
      }
      const auto feature_text = message.text(internal::field_id("features"));
      if (feature_text.has_value()) version.features = decode_features(*feature_text);
      const auto migration_text = message.text(internal::field_id("migrations"));
      if (migration_text.has_value()) {
        std::size_t start = 0;
        while (start < migration_text->size() && version.migrations.size() < limits::kMessageFields) {
          const std::size_t comma = migration_text->find(',', start);
          const std::size_t end = comma == std::string_view::npos ? migration_text->size() : comma;
          const std::string_view entry = migration_text->substr(start, end - start);
          const std::size_t first = entry.find(':');
          const std::size_t second = first == std::string_view::npos ? std::string_view::npos : entry.find(':', first + 1);
          bool ok = true;
          if (first != std::string_view::npos) {
            MigrationCapability capability;
            capability.id = MigrationId::from_valid(entry.substr(0, first));
            capability.schema = internal::component_state_schema_id();
            capability.source = SchemaGeneration::from_raw(parse_u64(entry.substr(first + 1, second - first - 1), ok));
            capability.target =
                second == std::string_view::npos
                    ? SchemaGeneration::unset()
                    : SchemaGeneration::from_raw(parse_u64(entry.substr(second + 1), ok));
            capability.reversible = true;
            capability.generation = MigrationGeneration::first();
            if (ok && !capability.id.empty()) version.migrations.push_back(capability);
          }
          if (comma == std::string_view::npos) break;
          start = comma + 1;
        }
      }
      version.evidence = EvidenceGeneration::unset();
      result = command_register_component(peer, version);
      response_type = MessageType::RegisterComponentAck;
      break;
    }
    case MessageType::PublishCapability: {
      const auto capability_text = message.text(internal::field_id("capability_generation"));
      bool ok = false;
      const std::uint64_t raw = capability_text.has_value() ? parse_u64(*capability_text, ok) : 0;
      (void)ok;
      const auto feature_text = message.text(internal::field_id("features"));
      result = command_publish_capability(
          peer, message.ident<RuntimeComponentIdTag, 63>(f_component),
          message.generation<RuntimeGenerationTag>(f_generation),
          CapabilityGeneration::from_raw(raw),
          feature_text.has_value() ? decode_features(*feature_text) : std::vector<FeatureSupport>{});
      response_type = MessageType::PublishCapabilityAck;
      break;
    }
    case MessageType::PublishCompatibility: {
      CompatibilityEdge edge;
      edge.from = message.generation<RuntimeGenerationTag>(internal::field_id("from_generation"));
      edge.to = message.generation<RuntimeGenerationTag>(internal::field_id("to_generation"));
      bool aspects_ok = true;
      for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
        std::string name = "aspect_";
        name += to_string(static_cast<CompatAspect>(i));
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto text = message.text(FieldId::from_valid(name));
        if (!text.has_value()) {
          aspects_ok = false;
          break;
        }
      }
      if (!aspects_ok) {
        result = internal::failure_result(
            Status::failure(ErrorCode::InvalidArgument, "compatibility edge must declare every aspect"));
        response_type = MessageType::PublishCompatibilityAck;
        break;
      }
      for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
        std::string name = "aspect_";
        name += to_string(static_cast<CompatAspect>(i));
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto text = message.text(FieldId::from_valid(name));
        const auto outcome = parse_compat_outcome(*text);
        if (!outcome.has_value()) {
          aspects_ok = false;
          break;
        }
        edge.aspects[i].outcome = *outcome;
        edge.aspects[i].provenance = EvidenceClass::Real;
        edge.aspects[i].evidence = EvidenceGeneration::first();
        edge.aspects[i].detail = DetailText::from_valid("published over the wire");
      }
      if (!aspects_ok) {
        result = internal::failure_result(
            Status::failure(ErrorCode::InvalidArgument, "compatibility aspect outcome is not recognised"));
        response_type = MessageType::PublishCompatibilityAck;
        break;
      }
      edge.permissions = static_cast<CompatPermissions>(message.u64(internal::field_id("permissions")).value_or(0));
      edge.requires_feature_gate = message.boolean(internal::field_id("requires_feature_gate")).value_or(false);
      edge.gate = message.ident<FeatureGateIdTag, 63>(internal::field_id("gate"));
      edge.gate_generation = message.generation<FeatureGateGenerationTag>(internal::field_id("gate_generation"));
      edge.requires_protocol_downgrade =
          message.boolean(internal::field_id("requires_protocol_downgrade")).value_or(false);
      edge.downgrade_to = message.generation<ProtocolGenerationTag>(internal::field_id("downgrade_to"));
      edge.requires_state_translation =
          message.boolean(internal::field_id("requires_state_translation")).value_or(false);
      edge.migration = message.ident<MigrationIdTag, 63>(internal::field_id("migration"));
      edge.migration_generation =
          message.generation<MigrationGenerationTag>(internal::field_id("migration_generation"));
      result = command_publish_compatibility(peer, edge);
      response_type = MessageType::PublishCompatibilityAck;
      break;
    }
    case MessageType::PublishSchema: {
      SchemaDescriptor descriptor;
      descriptor.id = message.ident<SchemaIdTag, 63>(internal::field_id("schema"));
      descriptor.generation = message.generation<SchemaGenerationTag>(internal::field_id("generation"));
      descriptor.readable_formats.min = message.generation<SchemaGenerationTag>(internal::field_id("readable_min"));
      descriptor.readable_formats.max = message.generation<SchemaGenerationTag>(internal::field_id("readable_max"));
      descriptor.writable_formats.min = message.generation<SchemaGenerationTag>(internal::field_id("writable_min"));
      descriptor.writable_formats.max = message.generation<SchemaGenerationTag>(internal::field_id("writable_max"));
      descriptor.reverse_migration_to =
          message.generation<SchemaGenerationTag>(internal::field_id("reverse_migration_to"));
      descriptor.provenance = EvidenceClass::Real;
      descriptor.evidence = EvidenceGeneration::first();
      const auto fields = message.text(internal::field_id("fields"));
      if (fields.has_value()) {
        std::size_t start = 0;
        while (start < fields->size() && descriptor.fields.size() < limits::kMessageFields) {
          const std::size_t comma = fields->find(',', start);
          const std::size_t end = comma == std::string_view::npos ? fields->size() : comma;
          const std::string_view entry = fields->substr(start, end - start);
          const std::size_t first = entry.find(':');
          const std::size_t second = first == std::string_view::npos ? std::string_view::npos : entry.find(':', first + 1);
          if (first != std::string_view::npos) {
            SchemaField field;
            field.id = FieldId::from_valid(entry.substr(0, first));
            const auto type = parse_field_type(entry.substr(first + 1, second - first - 1));
            field.type = type.has_value() ? *type : FieldType::U64;
            field.introduced_in = descriptor.generation;
            field.required = true;
            if (!field.id.empty()) descriptor.fields.push_back(field);
          }
          if (comma == std::string_view::npos) break;
          start = comma + 1;
        }
      }
      result = command_publish_schema(peer, descriptor);
      response_type = MessageType::PublishSchemaAck;
      break;
    }
    case MessageType::NegotiateProtocol: {
      ProtocolGenerationSet supported;
      const auto text = message.text(internal::field_id("supported_protocols"));
      if (text.has_value()) (void)decode_generation_set(*text, supported);
      result = command_negotiate(peer, supported);
      response_type = MessageType::NegotiateProtocolAck;
      break;
    }
    case MessageType::RequestUpgrade: {
      PlanSpec spec;
      spec.id = message.ident<EvolutionPlanIdTag, 63>(f_plan);
      spec.component = message.ident<RuntimeComponentIdTag, 63>(f_component);
      spec.candidate_generation = message.generation<RuntimeGenerationTag>(internal::field_id("candidate_generation"));
      spec.migration = message.ident<MigrationIdTag, 63>(internal::field_id("migration"));
      spec.migration_generation = message.generation<MigrationGenerationTag>(internal::field_id("migration_generation"));
      spec.migration_source = message.generation<SchemaGenerationTag>(internal::field_id("migration_source"));
      spec.migration_target = message.generation<SchemaGenerationTag>(internal::field_id("migration_target"));
      spec.canary_cohort = message.ident<CohortIdTag, 63>(internal::field_id("canary_cohort"));
      const auto cohorts = message.text(internal::field_id("cohorts"));
      if (cohorts.has_value()) {
        std::size_t start = 0;
        while (start < cohorts->size() && spec.cohorts.size() < limits::kCohorts) {
          const std::size_t comma = cohorts->find(',', start);
          const std::size_t end = comma == std::string_view::npos ? cohorts->size() : comma;
          const CohortId cohort = CohortId::from_valid(cohorts->substr(start, end - start));
          if (!cohort.empty()) spec.cohorts.push_back(cohort);
          if (comma == std::string_view::npos) break;
          start = comma + 1;
        }
      }
      spec.policy.require_canary = message.boolean(internal::field_id("policy_require_canary")).value_or(true);
      spec.policy.require_mixed_version_cohort =
          message.boolean(internal::field_id("policy_require_mixed_version")).value_or(true);
      spec.policy.require_migration_barrier =
          message.boolean(internal::field_id("policy_require_migration_barrier")).value_or(false);
      spec.policy.require_new_writer_barrier =
          message.boolean(internal::field_id("policy_require_new_writer_barrier")).value_or(true);
      spec.policy.require_old_writer_drain =
          message.boolean(internal::field_id("policy_require_old_writer_drain")).value_or(true);
      result = command_create_plan(peer, spec);
      response_type = MessageType::RequestUpgradeAck;
      break;
    }
    case MessageType::RequestDrain: {
      result = command_request_drain(peer, message.ident<RuntimeComponentIdTag, 63>(f_component),
                                     message.generation<RuntimeGenerationTag>(f_generation));
      response_type = MessageType::RequestDrainAck;
      break;
    }
    case MessageType::RequestRetirement: {
      result = command_request_retirement(peer, message.ident<RuntimeComponentIdTag, 63>(f_component),
                                          message.generation<RuntimeGenerationTag>(f_generation));
      response_type = MessageType::RequestRetirementAck;
      break;
    }
    case MessageType::PublishStage: {
      const auto stage_text = message.text(f_stage);
      const auto stage = stage_text.has_value() ? parse_rollout_stage(*stage_text) : std::nullopt;
      if (!stage.has_value()) {
        result = internal::failure_result(Status::failure(ErrorCode::InvalidArgument, "stage name is unknown"));
      } else {
        result = command_advance_stage(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan), *stage,
                                       message.generation<StageGenerationTag>(f_stage_generation));
      }
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::PublishCompletion: {
      const auto outcome_text = message.text(f_migration_outcome);
      MigrationOutcome outcome = MigrationOutcome::OutcomeUnknown;
      if (outcome_text.has_value()) {
        const auto parsed = parse_migration_outcome(*outcome_text);
        if (parsed.has_value()) outcome = *parsed;
      }
      IntegrityDigest digest;
      const auto digest_text = message.text(f_state_digest);
      if (digest_text.has_value()) {
        const auto parsed = IntegrityDigest::from_hex(*digest_text);
        if (parsed.has_value()) digest = *parsed;
      }
      result = command_publish_completion(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan),
                                          message.ident<RolloutStageIdTag, 63>(f_stage_id),
                                          message.generation<StageGenerationTag>(f_stage_generation), outcome,
                                          digest, message.boolean(internal::field_id("drain")).value_or(false));
      response_type = MessageType::PublishCompletionAck;
      break;
    }
    case MessageType::QueryState: {
      const auto query = message.text(f_query);
      result = command_query(peer, query.has_value() ? *query : std::string_view("authority"),
                             message.ident<RuntimeComponentIdTag, 63>(f_component));
      response_type = MessageType::QueryStateAck;
      break;
    }
    case MessageType::Fence: {
      const auto reason = message.text(f_reason);
      result = command_fence(peer, message.ident<WorkerIdTag, 63>(f_worker),
                             message.generation<WorkerBootIdTag>(f_boot),
                             reason.has_value() ? *reason : std::string_view("operator fence"));
      response_type = MessageType::FenceAck;
      break;
    }
    case MessageType::Heartbeat: {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      auto live = live_workers_.find(peer.worker);
      if (live == live_workers_.end()) {
        result = internal::failure_result(Status::failure(ErrorCode::StaleBoot, "worker is not current"));
      } else if (live->second.boot != peer.boot) {
        result = internal::failure_result(Status::failure(ErrorCode::StaleBoot, "boot identity mismatch"));
      } else {
        live->second.active = true;
        const auto capability_text = message.text(internal::field_id("capability_generation"));
        if (capability_text.has_value()) {
          bool ok = false;
          const std::uint64_t raw = parse_u64(*capability_text, ok);
          if (ok) live->second.capability_generation = CapabilityGeneration::from_raw(raw);
        }
        JsonWriter writer;
        writer.begin_object();
        writer.field("status", "OK");
        writer.field("detail", "heartbeat accepted");
        writer.field_generation("coordinator_epoch", durable_.coordinator_epoch);
        writer.field_generation("evolution_epoch", epoch_for_locked(peer.component));
        writer.end_object();
        result = internal::ok_json(writer.str(), "heartbeat accepted");
        (void)result.fields.set_text(internal::field_id("operation_class"),
                                     to_string(live->second.operation));
        (void)result.fields.set_generation(internal::field_id("negotiated_protocol"),
                                           live->second.negotiated_protocol);
        (void)result.fields.set_generation(internal::field_id("negotiated_schema"),
                                           live->second.committed_schema);
      }
      response_type = MessageType::HeartbeatAck;
      break;
    }
    case MessageType::Goodbye: {
      result = internal::ok_result("session closing");
      response_type = MessageType::Goodbye;
      break;
    }
    case MessageType::ReconcileReport: {
      std::string encoded;
      {
        internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
        const EvolutionPlanId plan_id = message.ident<EvolutionPlanIdTag, 63>(f_plan);
        const SchemaGeneration observed =
            message.generation<SchemaGenerationTag>(internal::field_id("state_generation"));
        for (auto& action : pending_actions_) {
          if (action.worker != peer.worker) continue;
          if (!plan_id.empty() && action.plan != plan_id) continue;
          if (action.state == ActionState::Acknowledged || action.state == ActionState::Abandoned) continue;
          const auto outcome_text = message.text(f_migration_outcome);
          MigrationOutcome outcome = MigrationOutcome::OutcomeUnknown;
          if (outcome_text.has_value()) {
            const auto parsed = parse_migration_outcome(*outcome_text);
            if (parsed.has_value()) outcome = *parsed;
          }
          // A worker reports facts; the coordinator draws the conclusion. An
          // observed state at the migration target means the commit happened
          // exactly once, whether or not the acknowledgement ever arrived.
          if (outcome == MigrationOutcome::OutcomeUnknown && observed.is_set()) {
            if (observed == action.target) {
              const SchemaDescriptor* target_schema =
                  durable_.schemas.find(internal::component_state_schema_id(), action.target);
              outcome = (target_schema != nullptr && target_schema->reverse_migration_to.is_set())
                            ? MigrationOutcome::RollbackAvailable
                            : MigrationOutcome::Irreversible;
            } else if (observed == action.source) {
              continue;  // nothing committed: the attempt stays outstanding
            }
          }
          const bool committed = outcome == MigrationOutcome::Committed ||
                                 outcome == MigrationOutcome::RollbackAvailable ||
                                 outcome == MigrationOutcome::Irreversible;
          if (!committed) continue;
          EvolutionPlan* plan = mutable_plan_locked(action.plan);
          if (plan == nullptr) continue;
          action.state = ActionState::Acknowledged;
          MigrationRecord record;
          record.id = action.migration;
          record.generation = action.migration_generation;
          record.schema = internal::component_state_schema_id();
          record.source = action.source;
          record.target = action.target;
          record.runtime_generation = plan->candidate_generation;
          record.epoch = plan->epoch;
          record.policy = plan->policy.generation;
          record.plan = plan->id;
          record.irreversible = outcome == MigrationOutcome::Irreversible;
          record.rollback_metadata_preserved = outcome == MigrationOutcome::RollbackAvailable;
          record.rollback_available = outcome == MigrationOutcome::RollbackAvailable;
          record.outcome = outcome;
          record.provenance = EvidenceClass::Real;
          record.recorded_at = plan->stage_generation;
          record.detail = DetailText::from_valid("reconciled after an ambiguous completion");
          if (durable_.migrations.size() < limits::kMigrationRecords) {
            durable_.migrations.push_back(record);
            if (outcome == MigrationOutcome::Irreversible) plan->rollback_barrier_crossed = true;
            ++stats_.migrations_committed;
          }
        }
        durable_.evidence_generation = durable_.evidence_generation.next();
        const Status encoded_status = encode_locked(encoded);
        if (encoded_status.is_failure()) {
          result = internal::failure_result(encoded_status);
        } else {
          result = internal::ok_result("reconciliation report recorded");
        }
      }
      if (result.is_ok()) {
        const Status stored = persist(encoded);
        if (stored.is_failure()) result = internal::failure_result(stored);
      }
      response_type = MessageType::PublishCompletionAck;
      break;
    }
    case MessageType::RequestStateTransform: {
      result = command_state_transform(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan),
                                       message.generation<SchemaGenerationTag>(f_target_schema));
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::RequestMigrationDispatch: {
      result = command_request_migration(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan));
      response_type = MessageType::RequestMigrationAck;
      break;
    }
    case MessageType::RequestRollbackDispatch: {
      result = command_request_rollback(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan));
      response_type = MessageType::RequestRollbackAck;
      break;
    }
    case MessageType::RequestSupersede: {
      result = command_supersede_plan(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan));
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::RequestFeatureEnable: {
      result = command_enable_feature(peer,
                                      message.ident<FeatureGateIdTag, 63>(internal::field_id("feature")),
                                      message.ident<EvolutionPlanIdTag, 63>(f_plan));
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::RequestLifecycle: {
      const auto lifecycle_text = message.text(internal::field_id("lifecycle"));
      const auto lifecycle = lifecycle_text.has_value() ? parse_lifecycle(*lifecycle_text) : std::nullopt;
      if (!lifecycle.has_value()) {
        result = internal::failure_result(
            Status::failure(ErrorCode::InvalidArgument, "lifecycle name is not recognised"));
      } else {
        result = command_request_lifecycle(peer, message.ident<RuntimeComponentIdTag, 63>(f_component),
                                           message.generation<RuntimeGenerationTag>(f_generation), *lifecycle);
      }
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::RequestPlanRebind: {
      result = command_rebind_plan(peer, message.ident<EvolutionPlanIdTag, 63>(f_plan),
                                   message.generation<EvolutionPlanGenerationTag>(
                                       internal::field_id("plan_generation")));
      response_type = MessageType::PublishStageAck;
      break;
    }
    case MessageType::RequestMigration:
    case MessageType::RequestRollback: {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.frames_rejected;
      result = internal::failure_result(Status::failure(          ErrorCode::Unauthorized, "message type is coordinator-to-worker only and is not accepted inbound"));
      response_type = MessageType::Error;
      break;
    }
    case MessageType::Hello:
    case MessageType::Invalid:
    default: {
      internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
      ++stats_.frames_rejected;
      result = internal::failure_result(
          Status::failure(ErrorCode::Unsupported, "message type is not accepted by the coordinator"));
      response_type = MessageType::Error;
      break;
    }
  }
  {
    internal::StateLockGuard guard(state_mutex_, state_lock_owner_);
    if (result.is_ok()) {
      ++stats_.events_processed;
    } else {
      ++stats_.events_rejected;
    }
  }
  respond(session, frame, response_type, result);
}

}  // namespace ref
