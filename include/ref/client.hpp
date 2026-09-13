// Runtime Evolution Fabric - evolution client.
//
// The client owns the connection to an EvolutionCoordinator, performs the
// protocol handshake, and stamps every request with the generations, boot
// identity and epochs that the coordinator negotiated. A client never silently
// upgrades itself: after a coordinator restart or a superseded plan it must
// renegotiate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ref/ids.hpp"
#include "ref/protocol.hpp"
#include "ref/support.hpp"
#include "ref/wire.hpp"

namespace ref {

struct ClientConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  bool operator_mode{false};
  NoteText role{};
  std::uint64_t sequence_seed{0};
};

struct HandshakeOutcome {
  Status status{};
  bool registered{false};
  ProtocolGeneration negotiated_protocol{};
  SchemaGeneration negotiated_schema{};
  OperationClass operation{OperationClass::None};
  EvolutionEpoch epoch{};
  CoordinatorEpoch coordinator_epoch{};
  StageGeneration stage_generation{};
  FeatureGateGeneration gate_generation{};
  CompatibilityGeneration matrix_generation{};
  RuntimeGeneration authoritative_generation{};
  bool new_writer_enabled{false};
  std::string detail{};
  std::string explanation{};

  [[nodiscard]] bool is_ok() const noexcept { return status.is_ok(); }
};

class EvolutionClient {
 public:
  explicit EvolutionClient(ClientConfig config);
  ~EvolutionClient();

  EvolutionClient(const EvolutionClient&) = delete;
  EvolutionClient& operator=(const EvolutionClient&) = delete;

  // Connects, sends HELLO and waits for HELLO_ACK.
  [[nodiscard]] Status connect(const ProtocolHandshake& handshake, HandshakeOutcome& outcome);
  void close() noexcept;
  [[nodiscard]] bool connected() const noexcept { return connected_; }

  // Sends a request stamped with the negotiated session state and waits for the
  // matching response. The response body is a status/detail/json document.
  [[nodiscard]] Status request(MessageType type, const WireMessage& payload, Frame& response,
                               WireMessage& body);
  // Sends a fully formed frame. Used by adversarial tests and by operators.
  [[nodiscard]] Status send_raw(const Frame& frame, Frame& response, WireMessage& body);

  [[nodiscard]] const PeerIdentity& peer() const noexcept { return peer_; }
  [[nodiscard]] ProtocolGeneration negotiated_protocol() const noexcept { return peer_.agreed_protocol; }
  [[nodiscard]] SchemaGeneration negotiated_schema() const noexcept { return peer_.agreed_schema; }
  [[nodiscard]] OperationClass operation_class() const noexcept { return peer_.operation; }
  [[nodiscard]] EvolutionEpoch evolution_epoch() const noexcept { return peer_.epoch; }
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept { return peer_.coordinator_epoch; }
  [[nodiscard]] const ClientConfig& config() const noexcept { return config_; }
  [[nodiscard]] std::uint64_t next_sequence() noexcept { return ++sequence_; }
  // Raw transport access for workers that must read coordinator-initiated
  // frames (drain, migration, rollback) on the same session.
  [[nodiscard]] TcpConnection& connection() noexcept { return connection_; }

  void set_peer(const PeerIdentity& peer) { peer_ = peer; }

 private:
  ClientConfig config_;
  TcpConnection connection_;
  PeerIdentity peer_{};
  std::uint64_t sequence_{0};
  bool connected_{false};
};

// Parses a HELLO_ACK body into a structured outcome.
[[nodiscard]] HandshakeOutcome parse_handshake_ack(const Frame& response, const WireMessage& body);

}  // namespace ref
