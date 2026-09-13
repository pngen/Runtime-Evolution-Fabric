// Runtime Evolution Fabric - bounded, versioned framing over real TCP.
//
// Every frame carries the generations it claims to speak, the boot identity of
// the sender and the controller epochs it believes are current. Validation
// happens before any mutation: magic, transport version, message type, flags,
// length, integrity, then generations, epochs and boot identity.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/ids.hpp"
#include "ref/protocol.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  RegisterComponent = 3,
  RegisterComponentAck = 4,
  PublishCapability = 5,
  PublishCapabilityAck = 6,
  PublishCompatibility = 7,
  PublishCompatibilityAck = 8,
  NegotiateProtocol = 9,
  NegotiateProtocolAck = 10,
  PublishSchema = 11,
  PublishSchemaAck = 12,
  RequestUpgrade = 13,
  RequestUpgradeAck = 14,
  RequestDrain = 15,
  RequestDrainAck = 16,
  RequestMigration = 17,
  RequestMigrationAck = 18,
  RequestRollback = 19,
  RequestRollbackAck = 20,
  RequestRetirement = 21,
  RequestRetirementAck = 22,
  PublishStage = 23,
  PublishStageAck = 24,
  PublishCompletion = 25,
  PublishCompletionAck = 26,
  QueryState = 27,
  QueryStateAck = 28,
  Fence = 29,
  FenceAck = 30,
  Heartbeat = 31,
  HeartbeatAck = 32,
  Error = 33,
  Goodbye = 34,
  ReconcileReport = 35,
  RequestStateTransform = 36,   // introduced in protocol generation 2
  RequestMigrationDispatch = 37, // operator: dispatch the plan's migration to a worker
  RequestRollbackDispatch = 38,  // operator: evaluate and commit a rollback
  RequestSupersede = 39,         // operator: supersede an in-progress plan
  RequestFeatureEnable = 40,     // operator: advance a feature gate under a plan
  RequestPlanRebind = 41,        // operator: rebind a plan to current generations
  RequestLifecycle = 42,         // operator: move a runtime generation's lifecycle
};

inline constexpr std::uint16_t kMaxMessageType = 42;
[[nodiscard]] const char* to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> parse_message_type(std::string_view text) noexcept;
// Protocol generation in which a message type first exists.
[[nodiscard]] ProtocolGeneration message_introduced_in(MessageType type) noexcept;
// Whether accepting the message changes authoritative state.
[[nodiscard]] bool message_is_mutating(MessageType type) noexcept;

// The field contract of a message type. This table is the single source of
// truth for what a message may and must carry: it is used to publish protocol
// descriptors and to validate incoming frames.
struct WireMessageSpec {
  MessageType type{MessageType::Invalid};
  bool mutating{false};
  bool operator_only{false};
  std::vector<const char*> fields{};
  std::vector<const char*> required{};
};

[[nodiscard]] const std::vector<WireMessageSpec>& wire_message_specs();
[[nodiscard]] const WireMessageSpec* wire_message_spec(MessageType type);
// Builds the descriptor of the built-in wire protocol at a generation.
[[nodiscard]] ProtocolDescriptor build_wire_protocol(ProtocolGeneration generation);

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kWireMagic = 0x52454631u;  // 'REF1'
inline constexpr std::uint16_t kWireTransportVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 76;
inline constexpr std::size_t kFramePayloadOffset = kFrameHeaderBytes;

enum FrameFlag : std::uint32_t {
  kFrameFlagNone = 0,
  kFrameFlagResponse = 1u << 0,
  kFrameFlagFatal = 1u << 1,
  kFrameFlagOperator = 1u << 2,
  kFrameFlagCompletion = 1u << 3,
};
inline constexpr std::uint32_t kKnownFrameFlags =
    kFrameFlagResponse | kFrameFlagFatal | kFrameFlagOperator | kFrameFlagCompletion;

struct FrameHeader {
  std::uint32_t magic{kWireMagic};
  std::uint16_t transport_version{kWireTransportVersion};
  MessageType type{MessageType::Invalid};
  std::uint32_t flags{kFrameFlagNone};
  std::uint32_t payload_length{0};
  RuntimeGeneration runtime_generation{};
  ProtocolGeneration protocol_generation{};
  SchemaGeneration schema_generation{};
  CoordinatorEpoch coordinator_epoch{};
  EvolutionEpoch evolution_epoch{};
  WorkerBootId boot{};
  std::uint64_t sequence{0};
  std::uint32_t crc{0};
};

struct Frame {
  FrameHeader header{};
  std::string payload{};

  [[nodiscard]] bool is_response() const noexcept { return (header.flags & kFrameFlagResponse) != 0; }
  [[nodiscard]] bool is_fatal() const noexcept { return (header.flags & kFrameFlagFatal) != 0; }
};

// Structural validation only: enough to reject hostile input before it is
// interpreted, without requiring any coordinator state.
[[nodiscard]] Status encode_frame(const Frame& frame, std::string& out);
[[nodiscard]] Status decode_frame(std::string_view bytes, Frame& out,
                                  std::size_t max_payload = limits::kFrameBytes);
// Validates magic, transport version, type, flags, length, integrity.
[[nodiscard]] Status validate_frame_envelope(const Frame& frame,
                                             std::size_t max_payload = limits::kFrameBytes);

// Identity of a connected peer, established by a successful handshake. It is
// session state only: it is never persisted and never restores authority.
struct PeerIdentity {
  SessionId session{};
  RuntimeComponentId component{};
  RuntimeVersionId version{};
  WorkerId worker{};
  WorkerBootId boot{};
  RuntimeGeneration runtime_generation{};
  ProtocolId protocol{};
  ProtocolGeneration agreed_protocol{};
  SchemaGeneration agreed_schema{};
  CapabilityGeneration capability_generation{};
  OperationClass operation{OperationClass::None};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  bool is_operator{false};
  bool registered{false};
  bool authenticated{false};
};

// ---------------------------------------------------------------------------
// Field-typed message body
// ---------------------------------------------------------------------------
struct WireField {
  FieldId name{};
  std::uint8_t kind{0};  // 0 = u64, 1 = text, 2 = bool, 3 = digest, 4 = document
  std::uint64_t number{0};
  std::string text{};
  IntegrityDigest digest{};

  [[nodiscard]] bool operator<(const WireField& other) const noexcept { return name < other.name; }
};

class WireMessage {
 public:
  WireMessage() = default;

  Status set_u64(const FieldId& name, std::uint64_t value);
  Status set_bool(const FieldId& name, bool value);
  Status set_text(const FieldId& name, std::string_view value);
  // Document fields carry generated JSON. They are bounded by the query bound,
  // which is larger than the plain text bound but still finite.
  Status set_document(const FieldId& name, std::string_view value);
  Status set_digest(const FieldId& name, IntegrityDigest value);
  template <class Tag>
  Status set_generation(const FieldId& name, Generation<Tag> value) {
    return set_u64(name, value.raw());
  }
  template <class Tag, std::uint16_t Capacity>
  Status set_ident(const FieldId& name, const Ident<Tag, Capacity>& value) {
    return set_text(name, value.view());
  }

  [[nodiscard]] bool has(const FieldId& name) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> u64(const FieldId& name) const noexcept;
  [[nodiscard]] std::optional<bool> boolean(const FieldId& name) const noexcept;
  [[nodiscard]] std::optional<std::string_view> text(const FieldId& name) const noexcept;
  [[nodiscard]] std::optional<std::string_view> document(const FieldId& name) const noexcept;
  [[nodiscard]] const WireField* field(const FieldId& name) const noexcept;
  [[nodiscard]] const std::vector<WireField>& fields() const noexcept { return fields_; }
  [[nodiscard]] std::size_t size() const noexcept { return fields_.size(); }

  template <class Tag>
  [[nodiscard]] Generation<Tag> generation(const FieldId& name) const noexcept {
    const auto value = u64(name);
    return value.has_value() ? Generation<Tag>::from_raw(*value) : Generation<Tag>::unset();
  }
  template <class Tag, std::uint16_t Capacity>
  [[nodiscard]] Ident<Tag, Capacity> ident(const FieldId& name) const noexcept {
    const auto value = text(name);
    if (!value.has_value()) return Ident<Tag, Capacity>{};
    return Ident<Tag, Capacity>::from_valid(*value);
  }

  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static Status decode(std::string_view bytes, WireMessage& out);

 private:
  Status assign(WireField value);
  std::vector<WireField> fields_{};  // canonical: sorted by field name
};

// Required-field helper used by every handler before it touches state.
[[nodiscard]] Status require_fields(const WireMessage& message, const std::vector<const char*>& names);

// Generation sets travel as comma separated text inside a bounded field.
template <class Tag>
[[nodiscard]] std::string encode_generation_set(const GenerationSet<Tag>& set) {
  std::string out;
  for (std::uint8_t i = 0; i < set.size(); ++i) {
    if (!out.empty()) out += ',';
    out += std::to_string(set.at(i).raw());
  }
  return out;
}

template <class Tag>
[[nodiscard]] Status decode_generation_set(std::string_view text, GenerationSet<Tag>& out) {
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view token = text.substr(start, end - start);
    if (!token.empty()) {
      bool ok = false;
      const std::uint64_t raw = parse_u64(token, ok);
      if (!ok) return Status::failure(ErrorCode::InvalidArgument, "generation set entry is not a number");
      if (!out.add(Generation<Tag>::from_raw(raw))) {
        return Status::failure(ErrorCode::LimitExceeded, "generation set exceeds its bound");
      }
    }
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Framed TCP transport
// ---------------------------------------------------------------------------
// Initialises the process-wide socket runtime on first use. It is never torn
// down while the process lives, so a component can always create a socket.
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::string error() const { return error_; }

 private:
  bool ok_{true};
  std::string error_{};
};

class TcpConnection {
 public:
  TcpConnection() = default;
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;
  void set_no_delay() noexcept;

  [[nodiscard]] Status send_all(std::string_view bytes);
  // Reads exactly 'count' bytes or fails. EOF is reported as Truncated.
  [[nodiscard]] Status recv_exact(char* buffer, std::size_t count);

  [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }
  void reset(std::uintptr_t handle) noexcept { handle_ = handle; }

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~0ull)};
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  [[nodiscard]] Status open(const std::string& bind_host, std::uint16_t port);
  [[nodiscard]] Status accept(TcpConnection& out);
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~0ull)};
  std::uint16_t port_{0};
};

[[nodiscard]] Status send_frame(TcpConnection& connection, const Frame& frame);
[[nodiscard]] Status recv_frame(TcpConnection& connection, Frame& out);
// Establishes an outbound TCP connection to a coordinator.
[[nodiscard]] Status connect_tcp(TcpConnection& out, const std::string& host, std::uint16_t port);

}  // namespace ref
