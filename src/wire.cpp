#include "ref/wire.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ref {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

void close_socket(NativeSocket socket) {
  if (socket != kInvalidSocket) (void)closesocket(socket);
}

#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void close_socket(NativeSocket socket) {
  if (socket != kInvalidSocket) (void)::close(socket);
}
#endif

NativeSocket to_native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }

constexpr std::array<const char*, kMaxMessageType + 1> kMessageNames{
    "INVALID",
    "HELLO",
    "HELLO_ACK",
    "REGISTER_COMPONENT",
    "REGISTER_COMPONENT_ACK",
    "PUBLISH_CAPABILITY",
    "PUBLISH_CAPABILITY_ACK",
    "PUBLISH_COMPATIBILITY",
    "PUBLISH_COMPATIBILITY_ACK",
    "NEGOTIATE_PROTOCOL",
    "NEGOTIATE_PROTOCOL_ACK",
    "PUBLISH_SCHEMA",
    "PUBLISH_SCHEMA_ACK",
    "REQUEST_UPGRADE",
    "REQUEST_UPGRADE_ACK",
    "REQUEST_DRAIN",
    "REQUEST_DRAIN_ACK",
    "REQUEST_MIGRATION",
    "REQUEST_MIGRATION_ACK",
    "REQUEST_ROLLBACK",
    "REQUEST_ROLLBACK_ACK",
    "REQUEST_RETIREMENT",
    "REQUEST_RETIREMENT_ACK",
    "PUBLISH_STAGE",
    "PUBLISH_STAGE_ACK",
    "PUBLISH_COMPLETION",
    "PUBLISH_COMPLETION_ACK",
    "QUERY_STATE",
    "QUERY_STATE_ACK",
    "FENCE",
    "FENCE_ACK",
    "HEARTBEAT",
    "HEARTBEAT_ACK",
    "ERROR",
    "GOODBYE",
    "RECONCILE_REPORT",
    "REQUEST_STATE_TRANSFORM",
    "REQUEST_MIGRATION_DISPATCH",
    "REQUEST_ROLLBACK_DISPATCH",
    "REQUEST_SUPERSEDE",
    "REQUEST_FEATURE_ENABLE",
    "REQUEST_PLAN_REBIND",
    "REQUEST_LIFECYCLE"};

}  // namespace

const char* to_string(MessageType type) noexcept {
  const auto index = static_cast<std::uint16_t>(type);
  return index <= kMaxMessageType ? kMessageNames[index] : "INVALID";
}

std::optional<MessageType> parse_message_type(std::string_view text) noexcept {
  for (std::uint16_t i = 0; i <= kMaxMessageType; ++i) {
    if (text == kMessageNames[i]) return static_cast<MessageType>(i);
  }
  return std::nullopt;
}

ProtocolGeneration message_introduced_in(MessageType type) noexcept {
  switch (type) {
    case MessageType::RequestStateTransform:
      return ProtocolGeneration::from_raw(2);
    case MessageType::Invalid:
      return ProtocolGeneration::unset();
    default:
      return ProtocolGeneration::from_raw(1);
  }
}

bool message_is_mutating(MessageType type) noexcept {
  switch (type) {
    case MessageType::RegisterComponent:
    case MessageType::PublishCapability:
    case MessageType::PublishCompatibility:
    case MessageType::PublishSchema:
    case MessageType::RequestUpgrade:
    case MessageType::RequestDrain:
    case MessageType::RequestMigration:
    case MessageType::RequestRollback:
    case MessageType::RequestRetirement:
    case MessageType::PublishStage:
    case MessageType::PublishCompletion:
    case MessageType::Fence:
    case MessageType::RequestStateTransform:
    case MessageType::RequestMigrationDispatch:
    case MessageType::RequestRollbackDispatch:
    case MessageType::RequestSupersede:
    case MessageType::RequestFeatureEnable:
    case MessageType::RequestPlanRebind:
    case MessageType::RequestLifecycle:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------
Status encode_frame(const Frame& frame, std::string& out) {
  if (frame.payload.size() > limits::kFrameBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "frame payload exceeds the frame bound");
  }
  if (frame.header.type == MessageType::Invalid) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot encode a frame with an invalid message type");
  }
  ByteWriter writer;
  writer.u32(kWireMagic);
  writer.u16(kWireTransportVersion);
  writer.u16(static_cast<std::uint16_t>(frame.header.type));
  writer.u32(frame.header.flags);
  writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
  writer.generation(frame.header.runtime_generation);
  writer.generation(frame.header.protocol_generation);
  writer.generation(frame.header.schema_generation);
  writer.generation(frame.header.coordinator_epoch);
  writer.generation(frame.header.evolution_epoch);
  writer.generation(frame.header.boot);
  writer.u64(frame.header.sequence);
  const std::uint32_t crc =
      crc32(std::string(writer.view()) + std::string(frame.payload));
  writer.u32(crc);
  writer.raw(frame.payload);
  out.assign(writer.view());
  return Status::ok();
}

Status decode_frame(std::string_view bytes, Frame& out, std::size_t max_payload) {
  out = Frame{};
  if (bytes.size() < kFrameHeaderBytes) {
    return Status::failure(ErrorCode::Truncated, "frame is shorter than the frame header");
  }
  if (bytes.size() > kFrameHeaderBytes + max_payload) {
    return Status::failure(ErrorCode::LimitExceeded, "frame exceeds the maximum frame size");
  }
  ByteReader reader(bytes);
  const std::uint32_t magic = reader.u32();
  const std::uint16_t transport_version = reader.u16();
  const std::uint16_t raw_type = reader.u16();
  const std::uint32_t flags = reader.u32();
  const std::uint32_t payload_length = reader.u32();
  FrameHeader header;
  header.runtime_generation = reader.generation<RuntimeGenerationTag>();
  header.protocol_generation = reader.generation<ProtocolGenerationTag>();
  header.schema_generation = reader.generation<SchemaGenerationTag>();
  header.coordinator_epoch = reader.generation<CoordinatorEpochTag>();
  header.evolution_epoch = reader.generation<EvolutionEpochTag>();
  header.boot = reader.generation<WorkerBootIdTag>();
  header.sequence = reader.u64();
  const std::size_t crc_offset = reader.consumed();
  const std::uint32_t stored_crc = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "frame header is truncated");

  if (magic != kWireMagic) return Status::failure(ErrorCode::Corrupt, "frame magic mismatch");
  if (transport_version != kWireTransportVersion) {
    return Status::failure(ErrorCode::Unsupported, "frame transport version is not supported");
  }
  if (raw_type == 0 || raw_type > kMaxMessageType) {
    return Status::failure(ErrorCode::Unsupported, "frame message type is unknown");
  }
  if ((flags & ~kKnownFrameFlags) != 0) {
    return Status::failure(ErrorCode::InvalidArgument, "frame carries unknown flags");
  }
  if (payload_length > max_payload) {
    return Status::failure(ErrorCode::LimitExceeded, "frame payload exceeds the maximum frame size");
  }
  if (reader.remaining() != payload_length) {
    return Status::failure(ErrorCode::Truncated, "frame payload length does not match the frame body");
  }
  // Integrity covers every header byte preceding the checksum plus the payload,
  // exactly as the encoder computes it.
  std::string covered;
  covered.reserve(crc_offset + payload_length);
  covered.append(bytes.data(), crc_offset);
  covered.append(bytes.data() + crc_offset + 4, payload_length);
  const std::uint32_t computed = crc32(covered);
  if (computed != stored_crc) {
    return Status::failure(ErrorCode::IntegrityFailure, "frame integrity check failed");
  }

  header.magic = magic;
  header.transport_version = transport_version;
  header.type = static_cast<MessageType>(raw_type);
  header.flags = flags;
  header.payload_length = payload_length;
  header.crc = stored_crc;
  out.header = header;
  out.payload.assign(bytes.substr(kFramePayloadOffset));
  return Status::ok();
}

Status validate_frame_envelope(const Frame& frame, std::size_t max_payload) {
  if (frame.header.magic != kWireMagic) {
    return Status::failure(ErrorCode::Corrupt, "frame magic mismatch");
  }
  if (frame.header.transport_version != kWireTransportVersion) {
    return Status::failure(ErrorCode::Unsupported, "frame transport version is not supported");
  }
  if (frame.header.type == MessageType::Invalid) {
    return Status::failure(ErrorCode::Unsupported, "frame message type is unknown");
  }
  if ((frame.header.flags & ~kKnownFrameFlags) != 0) {
    return Status::failure(ErrorCode::InvalidArgument, "frame carries unknown flags");
  }
  if (frame.payload.size() > max_payload) {
    return Status::failure(ErrorCode::LimitExceeded, "frame payload exceeds the maximum frame size");
  }
  if (frame.header.payload_length != frame.payload.size()) {
    return Status::failure(ErrorCode::Truncated, "frame payload length does not match the frame body");
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// WireMessage
// ---------------------------------------------------------------------------
Status WireMessage::assign(WireField value) {
  if (value.name.empty()) return Status::failure(ErrorCode::InvalidArgument, "wire field name is empty");
  const auto position =
      std::lower_bound(fields_.begin(), fields_.end(), value.name,
                       [](const WireField& entry, const FieldId& name) { return entry.name < name; });
  if (position != fields_.end() && position->name == value.name) {
    *position = std::move(value);
    return Status::ok();
  }
  if (fields_.size() >= limits::kMessageFields) {
    return Status::failure(ErrorCode::LimitExceeded, "wire message field bound reached");
  }
  fields_.insert(position, std::move(value));
  return Status::ok();
}

Status WireMessage::set_u64(const FieldId& name, std::uint64_t value) {
  WireField field;
  field.name = name;
  field.kind = 0;
  field.number = value;
  return assign(std::move(field));
}

Status WireMessage::set_bool(const FieldId& name, bool value) {
  WireField field;
  field.name = name;
  field.kind = 2;
  field.number = value ? 1u : 0u;
  return assign(std::move(field));
}

Status WireMessage::set_text(const FieldId& name, std::string_view value) {
  if (value.size() > limits::kTextBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "wire text field exceeds the text bound");
  }
  for (const char c : value) {
    if (!is_valid_text_char(c)) {
      return Status::failure(ErrorCode::InvalidArgument, "wire text field contains control characters");
    }
  }
  WireField field;
  field.name = name;
  field.kind = 1;
  field.text.assign(value);
  return assign(std::move(field));
}

Status WireMessage::set_document(const FieldId& name, std::string_view value) {
  if (value.size() > limits::kQueryBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "wire document field exceeds the document bound");
  }
  for (const char c : value) {
    if (!is_valid_text_char(c)) {
      return Status::failure(ErrorCode::InvalidArgument, "wire document field contains control characters");
    }
  }
  WireField field;
  field.name = name;
  field.kind = 4;
  field.text.assign(value);
  return assign(std::move(field));
}

Status WireMessage::set_digest(const FieldId& name, IntegrityDigest value) {
  WireField field;
  field.name = name;
  field.kind = 3;
  field.digest = value;
  return assign(std::move(field));
}

bool WireMessage::has(const FieldId& name) const noexcept { return field(name) != nullptr; }

const WireField* WireMessage::field(const FieldId& name) const noexcept {
  const auto position =
      std::lower_bound(fields_.begin(), fields_.end(), name,
                       [](const WireField& entry, const FieldId& key) { return entry.name < key; });
  if (position == fields_.end() || position->name != name) return nullptr;
  return &*position;
}

std::optional<std::uint64_t> WireMessage::u64(const FieldId& name) const noexcept {
  const WireField* entry = field(name);
  if (entry == nullptr || (entry->kind != 0 && entry->kind != 2)) return std::nullopt;
  return entry->number;
}

std::optional<bool> WireMessage::boolean(const FieldId& name) const noexcept {
  const WireField* entry = field(name);
  if (entry == nullptr || entry->kind != 2) return std::nullopt;
  return entry->number != 0;
}

std::optional<std::string_view> WireMessage::text(const FieldId& name) const noexcept {
  const WireField* entry = field(name);
  if (entry == nullptr || (entry->kind != 1 && entry->kind != 4)) return std::nullopt;
  return std::string_view(entry->text);
}

std::optional<std::string_view> WireMessage::document(const FieldId& name) const noexcept {
  const WireField* entry = field(name);
  if (entry == nullptr || entry->kind != 4) return std::nullopt;
  return std::string_view(entry->text);
}

std::string WireMessage::encode() const {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(fields_.size()));
  for (const auto& entry : fields_) {
    writer.ident(entry.name);
    writer.u8(entry.kind);
    switch (entry.kind) {
      case 0:
      case 2:
        writer.u64(entry.number);
        break;
      case 1:
        writer.text(entry.text, limits::kTextBytes);
        break;
      case 4:
        writer.text(entry.text, limits::kQueryBytes);
        break;
      case 3:
        writer.digest(entry.digest);
        break;
      default:
        break;
    }
  }
  return std::string(writer.view());
}

Status WireMessage::decode(std::string_view bytes, WireMessage& out) {
  out = WireMessage{};
  ByteReader reader(bytes);
  const std::uint32_t count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "wire message header is truncated");
  if (count > limits::kMessageFields) {
    return Status::failure(ErrorCode::LimitExceeded, "wire message declares too many fields");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const FieldId name = reader.ident<FieldIdTag, 63>();
    const std::uint8_t kind = reader.u8();
    if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "wire message field is malformed");
    if (kind > 4) return Status::failure(ErrorCode::Corrupt, "wire message field kind is unknown");
    Status status;
    switch (kind) {
      case 0: {
        const std::uint64_t value = reader.u64();
        status = out.set_u64(name, value);
        break;
      }
      case 2: {
        const std::uint64_t value = reader.u64();
        if (value > 1) return Status::failure(ErrorCode::Corrupt, "wire boolean field is not 0 or 1");
        status = out.set_bool(name, value == 1);
        break;
      }
      case 1: {
        const std::string_view value = reader.bytes(limits::kTextBytes);
        if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "wire text field is truncated");
        status = out.set_text(name, value);
        break;
      }
      case 4: {
        const std::string_view value = reader.bytes(limits::kQueryBytes);
        if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "wire document field is truncated");
        status = out.set_document(name, value);
        break;
      }
      case 3: {
        const IntegrityDigest value = reader.digest();
        status = out.set_digest(name, value);
        break;
      }
      default:
        return Status::failure(ErrorCode::Corrupt, "wire message field kind is unknown");
    }
    if (status.is_failure()) return status;
    if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "wire message field is malformed");
  }
  if (!reader.at_end()) return Status::failure(ErrorCode::Corrupt, "wire message has trailing bytes");
  return Status::ok();
}

Status require_fields(const WireMessage& message, const std::vector<const char*>& names) {
  for (const char* name : names) {
    const auto parsed = FieldId::parse(name);
    if (!parsed.has_value()) return Status::failure(ErrorCode::Internal, "handler declared an invalid field name");
    if (!message.has(*parsed)) {
      std::string detail = "required field is absent: ";
      detail += name;
      return Status::failure(ErrorCode::InvalidArgument, detail);
    }
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
namespace {

// Process-wide socket runtime. It is initialised once on first use and never
// torn down: a library must not de-initialise the network stack under another
// component that may still own live sockets.
bool ensure_socket_runtime(std::string& error) {
#if defined(_WIN32)
  static const bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!initialized) {
    error = "WSAStartup failed";
    return false;
  }
#else
  (void)error;
#endif
  return true;
}

}  // namespace

SocketRuntime::SocketRuntime() { ok_ = ensure_socket_runtime(error_); }

SocketRuntime::~SocketRuntime() = default;

bool TcpConnection::valid() const noexcept { return to_native(handle_) != kInvalidSocket; }

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept : handle_(other.handle_) {
  other.handle_ = static_cast<std::uintptr_t>(~0ull);
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = static_cast<std::uintptr_t>(~0ull);
  }
  return *this;
}

void TcpConnection::close() noexcept {
  if (to_native(handle_) != kInvalidSocket) {
    close_socket(to_native(handle_));
    handle_ = static_cast<std::uintptr_t>(~0ull);
  }
}

void TcpConnection::set_no_delay() noexcept {
  if (!valid()) return;
  const int one = 1;
#if defined(_WIN32)
  (void)setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                   sizeof(one));
#else
  (void)setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

Status TcpConnection::send_all(std::string_view bytes) {
  if (!valid()) return Status::failure(ErrorCode::Internal, "send on an invalid connection");
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t remaining = bytes.size() - sent;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#if defined(_WIN32)
    const int result = ::send(to_native(handle_), bytes.data() + sent, chunk, 0);
    if (result == SOCKET_ERROR) return Status::failure(ErrorCode::IoFailure, "socket send failed");
#else
    const ssize_t result = ::send(to_native(handle_), bytes.data() + sent, static_cast<std::size_t>(chunk), 0);
    if (result < 0) {
      if (errno == EINTR) continue;
      return Status::failure(ErrorCode::IoFailure, "socket send failed");
    }
#endif
    if (result == 0) return Status::failure(ErrorCode::IoFailure, "socket send returned zero");
    sent += static_cast<std::size_t>(result);
  }
  return Status::ok();
}

Status TcpConnection::recv_exact(char* buffer, std::size_t count) {
  if (!valid()) return Status::failure(ErrorCode::Internal, "receive on an invalid connection");
  std::size_t received = 0;
  while (received < count) {
    const std::size_t remaining = count - received;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#if defined(_WIN32)
    const int result = ::recv(to_native(handle_), buffer + received, chunk, 0);
    if (result == SOCKET_ERROR) return Status::failure(ErrorCode::IoFailure, "socket receive failed");
#else
    const ssize_t result = ::recv(to_native(handle_), buffer + received, static_cast<std::size_t>(chunk), 0);
    if (result < 0) {
      if (errno == EINTR) continue;
      return Status::failure(ErrorCode::IoFailure, "socket receive failed");
    }
#endif
    if (result == 0) return Status::failure(ErrorCode::Truncated, "peer closed the connection");
    received += static_cast<std::size_t>(result);
  }
  return Status::ok();
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::valid() const noexcept { return to_native(handle_) != kInvalidSocket; }

Status TcpListener::open(const std::string& bind_host, std::uint16_t port) {
  std::string socket_error;
  if (!ensure_socket_runtime(socket_error)) {
    return Status::failure(ErrorCode::Internal, socket_error);
  }
  close();
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return Status::failure(ErrorCode::IoFailure, "listening socket could not be created");
  const int one = 1;
#if defined(_WIN32)
  (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (bind_host.empty() || bind_host == "0.0.0.0") {
    address.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (bind_host == "127.0.0.1" || bind_host == "localhost") {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else {
    if (::inet_pton(AF_INET, bind_host.c_str(), &address.sin_addr) != 1) {
      close_socket(socket);
      return Status::failure(ErrorCode::InvalidArgument, "bind host is not a valid IPv4 address");
    }
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(socket);
    return Status::failure(ErrorCode::IoFailure, "listening socket could not bind");
  }
  if (::listen(socket, 64) != 0) {
    close_socket(socket);
    return Status::failure(ErrorCode::IoFailure, "listening socket could not listen");
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int length = sizeof(actual);
#else
  socklen_t length = sizeof(actual);
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
    port_ = ntohs(actual.sin_port);
  } else {
    port_ = port;
  }
  handle_ = static_cast<std::uintptr_t>(socket);
  return Status::ok();
}

Status TcpListener::accept(TcpConnection& out) {
  if (!valid()) return Status::failure(ErrorCode::Internal, "accept on an invalid listener");
  const NativeSocket socket = ::accept(to_native(handle_), nullptr, nullptr);
  if (socket == kInvalidSocket) {
    return Status::failure(ErrorCode::IoFailure, "accept failed");
  }
  out.close();
  out.reset(static_cast<std::uintptr_t>(socket));
  return Status::ok();
}

void TcpListener::close() noexcept {
  if (to_native(handle_) != kInvalidSocket) {
    close_socket(to_native(handle_));
    handle_ = static_cast<std::uintptr_t>(~0ull);
  }
}

Status send_frame(TcpConnection& connection, const Frame& frame) {
  std::string bytes;
  const Status encoded = encode_frame(frame, bytes);
  if (encoded.is_failure()) return encoded;
  return connection.send_all(bytes);
}

Status recv_frame(TcpConnection& connection, Frame& out) {
  std::array<char, kFrameHeaderBytes> header_bytes{};
  const Status header = connection.recv_exact(header_bytes.data(), header_bytes.size());
  if (header.is_failure()) return header;
  const std::string_view header_view(header_bytes.data(), header_bytes.size());
  ByteReader reader(header_view);
  (void)reader.u32();  // magic is re-validated below through the full frame
  (void)reader.u16();
  (void)reader.u16();
  (void)reader.u32();
  const std::uint32_t payload_length = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "frame header is truncated");
  if (payload_length > limits::kFrameBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "declared frame payload exceeds the frame bound");
  }
  std::string bytes(header_bytes.data(), header_bytes.size());
  if (payload_length > 0) {
    std::string payload(payload_length, '\0');
    const Status body = connection.recv_exact(payload.data(), payload_length);
    if (body.is_failure()) return body;
    bytes += payload;
  }
  return decode_frame(bytes, out);
}


// ---------------------------------------------------------------------------
// Message field contract
// ---------------------------------------------------------------------------
const std::vector<WireMessageSpec>& wire_message_specs() {
  static const std::vector<WireMessageSpec> specs{
      {MessageType::Hello,
       false,
       false,
       {"component", "version", "artifact", "runtime_generation", "worker", "boot", "protocol",
        "supported_protocols", "required_minimum", "supported_schemas", "committed_schema",
        "capability_generation", "evolution_epoch", "coordinator_epoch", "features", "declared_role"},
       {"component", "runtime_generation", "worker", "boot", "protocol", "supported_protocols",
        "supported_schemas", "capability_generation"}},
      {MessageType::HelloAck,
       false,
       false,
       {"status", "detail", "negotiated_protocol", "negotiated_schema", "operation_class",
        "evolution_epoch", "coordinator_epoch", "stage_generation", "gate_generation", "matrix_generation",
        "new_writer_enabled", "authoritative_generation", "features", "explanation", "registered",
        "rollout_stage"},
       {"status", "operation_class"}},
      {MessageType::RegisterComponent,
       true,
       false,
       {"component", "version", "artifact", "runtime_generation", "supported_protocols",
        "readable_protocols", "writable_protocols", "min_safety_protocol", "supported_schemas",
        "readable_schemas", "writable_schemas", "readable_formats", "writable_formats",
        "capability_generation", "lifecycle", "provenance", "features", "migrations"},
       {"component", "runtime_generation", "supported_protocols", "supported_schemas"}},
      {MessageType::RegisterComponentAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::PublishCapability,
       true,
       false,
       {"component", "runtime_generation", "capability_generation", "features"},
       {"component", "runtime_generation", "capability_generation"}},
      {MessageType::PublishCapabilityAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::PublishCompatibility,
       true,
       true,
       {"from_generation", "to_generation", "permissions", "requires_feature_gate", "gate",
        "gate_generation", "requires_protocol_downgrade", "downgrade_to", "requires_state_translation",
        "migration", "migration_generation", "provenance",
        "aspect_binary_api", "aspect_abi", "aspect_wire_protocol", "aspect_protocol_read",
        "aspect_protocol_write", "aspect_schema_read", "aspect_schema_write", "aspect_state_migration",
        "aspect_peer_version", "aspect_feature_gate", "aspect_persistence_format", "aspect_snapshot",
        "aspect_rollback", "aspect_capability"},
       {"from_generation", "to_generation", "permissions", "provenance"}},
      {MessageType::PublishCompatibilityAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::NegotiateProtocol,
       false,
       false,
       {"protocol", "supported_protocols", "required_minimum", "preferred"},
       {"protocol", "supported_protocols"}},
      {MessageType::NegotiateProtocolAck,
       false,
       false,
       {"status", "detail", "negotiated_protocol", "operation_class", "explanation", "downgraded"},
       {"status"}},
      {MessageType::PublishSchema,
       true,
       true,
       {"schema", "generation", "readable_min", "readable_max", "writable_min", "writable_max",
        "reverse_migration_to", "canonicalization", "integrity", "provenance", "fields"},
       {"schema", "generation", "provenance"}},
      {MessageType::PublishSchemaAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::RequestUpgrade,
       true,
       true,
       {"plan", "component", "candidate_generation", "policy_require_canary",
        "policy_require_mixed_version", "policy_require_migration_barrier",
        "policy_require_new_writer_barrier", "policy_require_old_writer_drain", "cohorts", "canary_cohort",
        "migration", "migration_generation", "migration_source", "migration_target"},
       {"component", "candidate_generation"}},
      {MessageType::RequestUpgradeAck, false, false, {"status", "detail", "plan", "stage"}, {"status"}},
      {MessageType::RequestDrain,
       true,
       false,
       {"component", "runtime_generation", "plan", "stage_generation"},
       {"component", "runtime_generation"}},
      {MessageType::RequestDrainAck,
       false,
       false,
       {"status", "detail", "live_processes", "active_workers"},
       {"status"}},
      {MessageType::RequestMigration,
       true,
       false,
       {"plan", "worker", "boot", "migration", "migration_generation", "source", "target", "action_id",
        "irreversible"},
       {"plan", "worker", "boot", "source", "target"}},
      {MessageType::RequestMigrationAck, false, false, {"status", "detail", "migration_outcome"}, {"status"}},
      {MessageType::RequestRollback,
       true,
       false,
       {"plan", "target_schema", "worker", "boot", "rollback_id"},
       {"plan"}},
      {MessageType::RequestRollbackAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::RequestRetirement,
       true,
       true,
       {"component", "runtime_generation", "checkpoints_retained"},
       {"component", "runtime_generation"}},
      {MessageType::RequestRetirementAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::PublishStage,
       true,
       true,
       {"plan", "stage", "stage_generation", "stage_id", "note"},
       {"plan", "stage", "stage_generation"}},
      {MessageType::PublishStageAck, false, false, {"status", "detail", "stage"}, {"status"}},
      {MessageType::PublishCompletion,
       true,
       false,
       {"plan", "stage_id", "stage_generation", "migration_outcome", "state_digest", "worker", "boot",
        "drain", "detail"},
       {"plan", "stage_id", "stage_generation"}},
      {MessageType::PublishCompletionAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::QueryState, false, false, {"component", "query"}, {"query"}},
      {MessageType::QueryStateAck, false, false, {"status", "detail", "json"}, {"status"}},
      {MessageType::Fence,
       true,
       true,
       {"worker", "boot", "reason", "component", "runtime_generation"},
       {"worker", "boot", "reason"}},
      {MessageType::FenceAck, false, false, {"status", "detail"}, {"status"}},
      {MessageType::Heartbeat,
       false,
       false,
       {"worker", "boot", "capability_generation", "committed_schema"},
       {"worker", "boot"}},
      {MessageType::HeartbeatAck,
       false,
       false,
       {"status", "detail", "coordinator_epoch", "evolution_epoch", "operation_class", "negotiated_protocol",
        "negotiated_schema"},
       {"status"}},
      {MessageType::Error, false, false, {"status", "detail"}, {"status"}},
      {MessageType::Goodbye, false, false, {"worker", "boot"}, {}},
      {MessageType::ReconcileReport,
       false,
       false,
       {"component", "json", "plan", "migration_outcome", "state_digest", "boot", "state_generation"},
       {"json"}},
      {MessageType::RequestStateTransform,
       true,
       false,
       {"plan", "target_schema", "worker", "boot", "feature"},
       {"plan", "target_schema", "worker", "boot"}},
      {MessageType::RequestMigrationDispatch, true, true, {"plan"}, {"plan"}},
      {MessageType::RequestRollbackDispatch, true, true, {"plan"}, {"plan"}},
      {MessageType::RequestSupersede, true, true, {"plan"}, {"plan"}},
      {MessageType::RequestFeatureEnable, true, true, {"plan", "feature"}, {"plan", "feature"}},
      {MessageType::RequestPlanRebind, true, true, {"plan", "plan_generation"}, {"plan"}},
      {MessageType::RequestLifecycle,
       true,
       true,
       {"component", "runtime_generation", "lifecycle"},
       {"component", "runtime_generation", "lifecycle"}},
  };
  return specs;
}

const WireMessageSpec* wire_message_spec(MessageType type) {
  for (const auto& spec : wire_message_specs()) {
    if (spec.type == type) return &spec;
  }
  return nullptr;
}

ProtocolDescriptor build_wire_protocol(ProtocolGeneration generation) {
  ProtocolDescriptor descriptor;
  descriptor.id = ProtocolId::from_valid("ref-wire");
  descriptor.generation = generation;
  descriptor.minimum_safety_generation = ProtocolGeneration::from_raw(1);
  descriptor.unknown_fields = UnknownFieldPolicy::Reject;
  descriptor.unknown_messages = UnknownMessagePolicy::Reject;
  descriptor.provenance = EvidenceClass::Real;
  descriptor.evidence = EvidenceGeneration::first();
  descriptor.notes = NoteText::from_valid(generation.raw() == 1
                                              ? "baseline wire protocol"
                                              : "wire protocol with state transformation messages");
  for (const auto& spec : wire_message_specs()) {
    if (message_introduced_in(spec.type) > generation) continue;
    MessageTypeDescriptor message;
    message.id = MessageTypeId::from_valid(to_string(spec.type));
    message.introduced_in = message_introduced_in(spec.type);
    message.mutating = spec.mutating;
    message.state_replacing = false;
    for (const char* field : spec.fields) {
      FieldDescriptor field_descriptor;
      field_descriptor.id = FieldId::from_valid(field);
      field_descriptor.introduced_in = message_introduced_in(spec.type);
      field_descriptor.required =
          std::find_if(spec.required.begin(), spec.required.end(), [field](const char* required) {
            return std::string_view(required) == std::string_view(field);
          }) != spec.required.end();
      message.fields.push_back(field_descriptor);
    }
    descriptor.messages.push_back(message);
  }
  return descriptor;
}

Status connect_tcp(TcpConnection& out, const std::string& host, std::uint16_t port) {
  std::string socket_error;
  if (!ensure_socket_runtime(socket_error)) {
    return Status::failure(ErrorCode::Internal, socket_error);
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return Status::failure(ErrorCode::IoFailure, "client socket could not be created");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  const std::string target = host.empty() ? std::string("127.0.0.1") : host;
  if (::inet_pton(AF_INET, target.c_str(), &address.sin_addr) != 1) {
    close_socket(socket);
    return Status::failure(ErrorCode::InvalidArgument, "coordinator host is not a valid IPv4 address");
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(socket);
    return Status::failure(ErrorCode::IoFailure, "connection to the coordinator failed");
  }
  out.close();
  out.reset(static_cast<std::uintptr_t>(socket));
  return Status::ok();
}

}  // namespace ref