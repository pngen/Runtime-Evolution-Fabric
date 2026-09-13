// Runtime Evolution Fabric - persistence and protocol adversarial tests.
//
// Every case here attacks durable state or a frame and asserts that the failure
// is detected before it can mutate anything.
#include <fstream>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace ref;
using reftest::TempDir;

namespace {

std::string read_all(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

DurableState make_state() {
  DurableState state;
  state.coordinator_epoch = CoordinatorEpoch::first();
  state.evolution_epoch = EvolutionEpoch::first();
  state.evidence_generation = EvidenceGeneration::first();
  state.stage_generation = StageGeneration::first();
  state.policy_generation = PolicyGeneration::first();
  state.writer_version.number = VersionNumber{1, 0, 0};
  state.writer_version.artifact.build = BuildId::from_valid("refcoordinator");
  (void)install_builtin_state_schemas(state.schemas);
  (void)state.protocols.publish(build_wire_protocol(ProtocolGeneration::from_raw(1)));
  (void)state.protocols.publish(build_wire_protocol(ProtocolGeneration::from_raw(2)));
  (void)state.components.publish(reftest::make_component("orders", 1, "1", "1", LifecycleState::Current));
  (void)state.components.publish(reftest::make_component("orders", 2, "1,2", "1,2", LifecycleState::Eligible,
                                                         {{1, 2}, {2, 1}}));
  (void)state.matrix.upsert(reftest::make_edge(1, 2, "mixed-version"));
  state.matrix_generation = state.matrix.generation();
  state.gate_generation = state.gates.generation();
  WorkerRecord worker;
  worker.worker = WorkerId::from_valid("worker-a");
  worker.component = RuntimeComponentId::from_valid("orders");
  worker.generation = RuntimeGeneration::from_raw(1);
  worker.last_boot = WorkerBootId::from_raw(1);
  state.workers.emplace(worker.worker, worker);
  FencedBoot fenced;
  fenced.worker = worker.worker;
  fenced.boot = WorkerBootId::from_raw(9);
  fenced.component = worker.component;
  fenced.generation = worker.generation;
  fenced.reason = NoteText::from_valid("test fence");
  state.fenced_boots.emplace(std::make_pair(fenced.worker, fenced.boot.raw()), fenced);
  return state;
}

}  // namespace

REF_TEST(store, round_trip_preserves_every_section) {
  TempDir dir("store");
  const std::string path = dir.file("evolution.state");
  const DurableState state = make_state();
  DurableStore store(path);
  REF_CHECK_STATUS_OK(store.save(state));
  DurableState loaded;
  REF_CHECK_STATUS_OK(store.load(loaded));
  REF_CHECK_EQ(loaded.components.version_count(), state.components.version_count());
  REF_CHECK_EQ(loaded.matrix.edge_count(), state.matrix.edge_count());
  REF_CHECK_EQ(loaded.protocols.size(), state.protocols.size());
  REF_CHECK_EQ(loaded.schemas.size(), state.schemas.size());
  REF_CHECK_EQ(loaded.fenced_boots.size(), 1u);
  REF_CHECK_EQ(loaded.workers.size(), 1u);
  REF_CHECK(loaded.is_boot_fenced(WorkerId::from_valid("worker-a"), WorkerBootId::from_raw(9)));
  REF_CHECK(!loaded.is_boot_fenced(WorkerId::from_valid("worker-a"), WorkerBootId::from_raw(1)));
  REF_CHECK_EQ(loaded.coordinator_epoch.raw(), state.coordinator_epoch.raw());
  REF_CHECK_EQ(loaded.matrix_generation.raw(), state.matrix_generation.raw());
  std::string reencoded;
  REF_CHECK_STATUS_OK(DurableStore::encode(loaded, reencoded));
  std::string original;
  REF_CHECK_STATUS_OK(DurableStore::encode(state, original));
  REF_CHECK_EQ(original, reencoded);  // canonical: same state, same bytes
}

REF_TEST(store, adversarial_inputs_are_rejected_without_partial_apply) {
  const DurableState state = make_state();
  std::string encoded;
  REF_CHECK_STATUS_OK(DurableStore::encode(state, encoded));
  auto expect_corrupt = [&](const std::string& bytes, const char* what) {
    DurableState target;
    const Status status = DurableStore::decode(bytes, target);
    ++reftest::g_checks;
    if (status.is_ok()) {
      reftest::report_failure(__FILE__, __LINE__, std::string("attack succeeded: ") + what);
    }
    // No partial apply: the destination is untouched on failure.
    ++reftest::g_checks;
    if (target.components.version_count() != 0 || target.matrix.edge_count() != 0) {
      reftest::report_failure(__FILE__, __LINE__, std::string("partial apply after: ") + what);
    }
  };

  expect_corrupt("", "empty file");
  expect_corrupt(encoded.substr(0, 4), "truncated header");
  expect_corrupt(encoded.substr(0, encoded.size() / 2), "truncated body");
  std::string wrong_magic = encoded;
  wrong_magic[1] = 'X';
  expect_corrupt(wrong_magic, "wrong magic");
  std::string wrong_version = encoded;
  wrong_version[8] = static_cast<char>(9);
  expect_corrupt(wrong_version, "unsupported format version");
  std::string corrupt_body = encoded;
  corrupt_body[corrupt_body.size() - 3] = static_cast<char>(corrupt_body[corrupt_body.size() - 3] ^ 0x5A);
  expect_corrupt(corrupt_body, "corrupt body byte");
  std::string corrupt_crc = encoded;
  corrupt_crc[8 + 4 + 8] = static_cast<char>(corrupt_crc[8 + 4 + 8] ^ 0x11);
  expect_corrupt(corrupt_crc, "corrupt header checksum");
  std::string trailing = encoded + "extra";
  expect_corrupt(trailing, "trailing bytes");
  expect_corrupt(std::string(64, '\0'), "zero filled file");
}

REF_TEST(store, oversized_counts_and_impossible_records_are_rejected) {
  const DurableState state = make_state();
  std::string encoded;
  REF_CHECK_STATUS_OK(DurableStore::encode(state, encoded));

  // The durable body starts after the fixed header; rewriting the component
  // count with an absurd value must be rejected rather than allocated.
  const std::size_t body_offset = 8 + 4 + 8 + 4 + 16;
  std::string huge = encoded;
  for (std::size_t i = 0; i < 4; ++i) {
    huge[body_offset + 3 + 6 + 11 * 8 + i] = static_cast<char>(0xFF);  // component count word
  }
  DurableState target;
  const Status status = DurableStore::decode(huge, target);
  REF_CHECK(status.is_failure());
  REF_CHECK_EQ(target.components.version_count(), 0u);
}

REF_TEST(store, cross_section_contradictions_are_rejected) {
  // A retired generation that is also CURRENT is contradictory.
  DurableState state = make_state();
  RetirementRecord retirement;
  retirement.component = RuntimeComponentId::from_valid("orders");
  retirement.generation = RuntimeGeneration::from_raw(1);
  retirement.recorded_at = StageGeneration::first();
  retirement.epoch = EvolutionEpoch::first();
  retirement.coordinator_epoch = CoordinatorEpoch::first();
  state.retirements.emplace(std::make_pair(retirement.component, retirement.generation.raw()), retirement);
  std::string encoded;
  REF_CHECK_STATUS_CODE(DurableStore::encode(state, encoded), ErrorCode::Corrupt);

  // A migration record that is irreversible and claims rollback availability.
  DurableState second = make_state();
  MigrationRecord record;
  record.id = MigrationId::from_valid("migration-2-3");
  record.generation = MigrationGeneration::first();
  record.schema = SchemaId::from_valid("component-state");
  record.source = SchemaGeneration::from_raw(2);
  record.target = SchemaGeneration::from_raw(3);
  record.runtime_generation = RuntimeGeneration::from_raw(2);
  record.epoch = EvolutionEpoch::first();
  record.irreversible = true;
  record.rollback_available = true;
  record.outcome = MigrationOutcome::Irreversible;
  second.migrations.push_back(record);
  REF_CHECK_STATUS_CODE(DurableStore::encode(second, encoded), ErrorCode::Corrupt);

  // A migration whose source equals its target is not a migration.
  DurableState third = make_state();
  record.irreversible = false;
  record.rollback_available = false;
  record.source = SchemaGeneration::from_raw(2);
  record.target = SchemaGeneration::from_raw(2);
  third.migrations.push_back(record);
  REF_CHECK_STATUS_CODE(DurableStore::encode(third, encoded), ErrorCode::Corrupt);
}

REF_TEST(store, generation_mismatch_between_counters_and_registries_is_rejected) {
  DurableState state = make_state();
  state.matrix_generation = CompatibilityGeneration::from_raw(99);
  std::string encoded;
  REF_CHECK_STATUS_CODE(DurableStore::encode(state, encoded), ErrorCode::Corrupt);
}

REF_TEST(store, failed_load_does_not_disturb_the_previous_state) {
  TempDir dir("store-fail");
  const std::string path = dir.file("evolution.state");
  const DurableState state = make_state();
  DurableStore store(path);
  REF_CHECK_STATUS_OK(store.save(state));
  DurableState previous;
  REF_CHECK_STATUS_OK(store.load(previous));
  const std::size_t versions = previous.components.version_count();
  write_all(path, "not a durable state file");
  const Status reload = store.load(previous);
  REF_CHECK(reload.is_failure());
  REF_CHECK_EQ(previous.components.version_count(), versions);
}

// ---------------------------------------------------------------------------
// Protocol adversarial tests
// ---------------------------------------------------------------------------
namespace {

Frame make_frame(MessageType type, const std::string& payload = std::string()) {
  Frame frame;
  frame.header.type = type;
  frame.header.runtime_generation = RuntimeGeneration::from_raw(1);
  frame.header.protocol_generation = ProtocolGeneration::from_raw(1);
  frame.header.schema_generation = SchemaGeneration::from_raw(1);
  frame.header.coordinator_epoch = CoordinatorEpoch::first();
  frame.header.evolution_epoch = EvolutionEpoch::first();
  frame.header.boot = WorkerBootId::from_raw(1);
  frame.header.sequence = 1;
  frame.payload = payload;
  return frame;
}

void expect_frame_rejected(const std::string& bytes, const char* what,
                           std::optional<ErrorCode> expected = std::nullopt) {
  Frame frame;
  const Status status = decode_frame(bytes, frame);
  ++reftest::g_checks;
  if (status.is_ok()) {
    reftest::report_failure(__FILE__, __LINE__, std::string("frame attack succeeded: ") + what);
    return;
  }
  if (expected.has_value()) {
    ++reftest::g_checks;
    if (status.code() != *expected) {
      reftest::report_failure(__FILE__, __LINE__,
                              std::string("wrong rejection code for ") + what + ": " + status.to_string());
    }
  }
  ++reftest::g_checks;
  if (frame.header.magic != 0 && frame.payload.size() != 0) {
    reftest::report_failure(__FILE__, __LINE__, std::string("payload visible after: ") + what);
  }
}

}  // namespace

REF_TEST(wire, valid_frame_round_trips) {
  WireMessage body;
  (void)body.set_text(FieldId::from_valid("query"), "components");
  const Frame frame = make_frame(MessageType::QueryState, body.encode());
  std::string bytes;
  REF_CHECK_STATUS_OK(encode_frame(frame, bytes));
  REF_CHECK_EQ(bytes.size(), kFrameHeaderBytes + frame.payload.size());
  Frame decoded;
  REF_CHECK_STATUS_OK(decode_frame(bytes, decoded));
  REF_CHECK_EQ(decoded.header.type, MessageType::QueryState);
  REF_CHECK_EQ(decoded.header.sequence, 1ull);
  REF_CHECK_EQ(decoded.header.boot.raw(), 1ull);
  WireMessage decoded_body;
  REF_CHECK_STATUS_OK(WireMessage::decode(decoded.payload, decoded_body));
  REF_CHECK_EQ(*decoded_body.text(FieldId::from_valid("query")), std::string_view("components"));
}

REF_TEST(wire, adversarial_frames_are_rejected_before_interpretation) {
  WireMessage body;
  (void)body.set_text(FieldId::from_valid("query"), "components");
  const Frame frame = make_frame(MessageType::QueryState, body.encode());
  std::string bytes;
  REF_CHECK_STATUS_OK(encode_frame(frame, bytes));

  expect_frame_rejected("", "empty frame", ErrorCode::Truncated);
  expect_frame_rejected(bytes.substr(0, kFrameHeaderBytes - 1), "truncated header", ErrorCode::Truncated);
  expect_frame_rejected(bytes.substr(0, bytes.size() - 1), "truncated payload", ErrorCode::Truncated);

  std::string wrong_magic = bytes;
  wrong_magic[0] = 'X';
  expect_frame_rejected(wrong_magic, "wrong magic", ErrorCode::Corrupt);

  std::string wrong_version = bytes;
  wrong_version[4] = 9;
  expect_frame_rejected(wrong_version, "wrong transport version", ErrorCode::Unsupported);

  std::string unknown_type = bytes;
  unknown_type[6] = static_cast<char>(0x7F);
  expect_frame_rejected(unknown_type, "unknown message type", ErrorCode::Unsupported);

  std::string unknown_flags = bytes;
  unknown_flags[8] = static_cast<char>(0x80);
  expect_frame_rejected(unknown_flags, "unknown flags", ErrorCode::InvalidArgument);

  std::string oversized = bytes;
  for (std::size_t i = 0; i < 4; ++i) oversized[12 + i] = static_cast<char>(0xFF);
  expect_frame_rejected(oversized, "oversized payload length", ErrorCode::LimitExceeded);

  std::string corrupted = bytes;
  corrupted[kFrameHeaderBytes] = static_cast<char>(corrupted[kFrameHeaderBytes] ^ 0x33);
  expect_frame_rejected(corrupted, "integrity failure", ErrorCode::IntegrityFailure);

  // A frame longer than the configured maximum is refused outright.
  Frame big = make_frame(MessageType::QueryState, std::string(limits::kFrameBytes + 1, 'a'));
  std::string big_bytes;
  REF_CHECK_STATUS_CODE(encode_frame(big, big_bytes), ErrorCode::LimitExceeded);
}

REF_TEST(wire, message_field_contract_is_enforced) {
  WireMessage message;
  (void)message.set_text(FieldId::from_valid("status"), "OK");
  const Status missing = require_fields(message, {"status", "detail"});
  REF_CHECK_STATUS_CODE(missing, ErrorCode::InvalidArgument);
  (void)message.set_text(FieldId::from_valid("detail"), "fine");
  REF_CHECK_STATUS_OK(require_fields(message, {"status", "detail"}));
  // Control characters never enter a canonical text field.
  REF_CHECK_STATUS_CODE(message.set_text(FieldId::from_valid("detail"), "line\nbreak"),
                        ErrorCode::InvalidArgument);
  REF_CHECK_STATUS_CODE(message.set_text(FieldId::from_valid("detail"), std::string(600, 'x')),
                        ErrorCode::LimitExceeded);
  // A document field is larger but still bounded.
  const std::string document(limits::kTextBytes + 10, 'y');
  REF_CHECK_STATUS_OK(message.set_document(FieldId::from_valid("json"), document));
  REF_CHECK_STATUS_CODE(message.set_document(FieldId::from_valid("json"),
                                             std::string(limits::kQueryBytes + 1, 'z')),
                        ErrorCode::LimitExceeded);
}

REF_TEST(wire, malformed_message_bodies_are_rejected) {
  WireMessage body;
  (void)body.set_u64(FieldId::from_valid("boot"), 1);
  const std::string encoded = body.encode();
  WireMessage decoded;
  REF_CHECK_STATUS_OK(WireMessage::decode(encoded, decoded));
  REF_CHECK_STATUS_CODE(WireMessage::decode(encoded.substr(0, encoded.size() - 1), decoded),
                        ErrorCode::Corrupt);
  REF_CHECK_STATUS_CODE(WireMessage::decode(encoded + "trailing", decoded), ErrorCode::Corrupt);
  // A boolean field must be exactly 0 or 1.
  ByteWriter writer;
  writer.u32(1);
  writer.ident(FieldId::from_valid("flag"));
  writer.u8(2);  // bool kind
  writer.u64(7);
  REF_CHECK_STATUS_CODE(WireMessage::decode(std::string(writer.view()), decoded), ErrorCode::Corrupt);
  // An unknown field kind is refused.
  ByteWriter unknown;
  unknown.u32(1);
  unknown.ident(FieldId::from_valid("flag"));
  unknown.u8(9);
  unknown.u64(1);
  REF_CHECK_STATUS_CODE(WireMessage::decode(std::string(unknown.view()), decoded), ErrorCode::Corrupt);
}

REF_TEST(wire, gossip_generation_set_encoding_is_bounded) {
  ProtocolGenerationSet set;
  (void)set.add(ProtocolGeneration::from_raw(1));
  (void)set.add(ProtocolGeneration::from_raw(2));
  const std::string encoded = encode_generation_set(set);
  REF_CHECK_EQ(encoded, std::string("1,2"));
  ProtocolGenerationSet decoded;
  REF_CHECK_STATUS_OK(decode_generation_set(encoded, decoded));
  REF_CHECK(decoded == set);
  ProtocolGenerationSet bad;
  REF_CHECK_STATUS_CODE(decode_generation_set("1,two", bad), ErrorCode::InvalidArgument);
  REF_CHECK_STATUS_CODE(decode_generation_set("1,nope", bad), ErrorCode::InvalidArgument);
}
