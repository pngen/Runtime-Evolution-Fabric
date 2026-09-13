// Runtime Evolution Fabric - core unit tests: identities, codec, compatibility,
// protocol negotiation and schema read/write separation.
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace ref;
using reftest::make_component;
using reftest::make_edge;
using reftest::make_protocol_registry;
using reftest::make_schema_registry;
using reftest::TempDir;

namespace {

bool has_capacity_overflow() { return false; }

}  // namespace

REF_TEST(core, ident_validation_and_strong_typing) {
  const auto ok = RuntimeComponentId::parse("orders");
  REF_CHECK(ok.has_value());
  REF_CHECK_EQ(ok->view(), std::string_view("orders"));
  REF_CHECK(!RuntimeComponentId::parse("").has_value());
  REF_CHECK(!RuntimeComponentId::parse("has space").has_value());
  REF_CHECK(!RuntimeComponentId::parse("control\nchar").has_value());
  const std::string too_long(64, 'a');
  REF_CHECK(!RuntimeComponentId::parse(too_long).has_value());
  const std::string at_limit(63, 'a');
  REF_CHECK(RuntimeComponentId::parse(at_limit).has_value());
}

REF_TEST(core, generation_arithmetic_and_staleness) {
  const RuntimeGeneration first = RuntimeGeneration::first();
  REF_CHECK_EQ(first.raw(), 1ull);
  REF_CHECK(!RuntimeGeneration::unset().is_set());
  REF_CHECK(first.next() > first);
  REF_CHECK(first.is_stale_relative_to(first.next()));
  const auto ceiling = RuntimeGeneration::from_raw(kGenerationCeiling);
  REF_CHECK_EQ(ceiling.next().raw(), kGenerationCeiling);  // saturated, never wraps
  REF_CHECK(!checked_add(kGenerationCeiling, 1, *new std::uint64_t(0)));
  std::uint64_t sum = 0;
  REF_CHECK(checked_add(2, 3, sum));
  REF_CHECK_EQ(sum, 5ull);
  REF_CHECK(!checked_mul(kGenerationCeiling, 2, sum));
}

REF_TEST(core, generation_sets_are_sorted_and_bounded) {
  ProtocolGenerationSet set;
  (void)set.add(ProtocolGeneration::from_raw(3));
  (void)set.add(ProtocolGeneration::from_raw(1));
  (void)set.add(ProtocolGeneration::from_raw(2));
  REF_CHECK_EQ(set.size(), 3);
  REF_CHECK_EQ(set.lowest().raw(), 1ull);
  REF_CHECK_EQ(set.highest().raw(), 3ull);
  REF_CHECK_EQ(set.highest_at_most(ProtocolGeneration::from_raw(2)).raw(), 2ull);
  REF_CHECK(set.contains(ProtocolGeneration::from_raw(2)));
  REF_CHECK(!set.contains(ProtocolGeneration::from_raw(4)));
  ProtocolGenerationSet filled;
  bool overflowed = false;
  for (std::uint64_t i = 1; i <= kMaxGenerationsPerSet + 4; ++i) {
    if (!filled.add(ProtocolGeneration::from_raw(i))) overflowed = true;
  }
  REF_CHECK(overflowed);
  REF_CHECK_EQ(filled.size(), kMaxGenerationsPerSet);
}

REF_TEST(core, canonical_codec_rejects_truncation_and_excess) {
  ByteWriter writer;
  writer.u32(7);
  writer.text("hello", 16);
  ByteReader reader(writer.view());
  REF_CHECK_EQ(reader.u32(), 7u);
  const auto text = reader.bytes(16);
  REF_CHECK_EQ(text, std::string_view("hello"));
  REF_CHECK(reader.at_end());
  REF_CHECK(!reader.failed());

  ByteReader truncated(writer.view().substr(0, 3));
  (void)truncated.u32();
  REF_CHECK(truncated.failed());

  // A reader whose bound is smaller than the encoded field must fail closed.
  ByteWriter oversized;
  oversized.text(std::string(64, 'x'), 64);
  ByteReader bounded(oversized.view());
  (void)bounded.bytes(8);
  REF_CHECK(bounded.failed());
}

REF_TEST(core, digest_is_deterministic_and_hex_round_trips) {
  const IntegrityDigest first = IntegrityDigest::of("payload");
  const IntegrityDigest second = IntegrityDigest::of("payload");
  REF_CHECK(first == second);
  REF_CHECK(first != IntegrityDigest::of("payloa"));
  const std::string hex = first.to_hex();
  REF_CHECK_EQ(hex.size(), 32u);
  const auto parsed = IntegrityDigest::from_hex(hex);
  REF_CHECK(parsed.has_value());
  REF_CHECK(*parsed == first);
  REF_CHECK(!IntegrityDigest::from_hex("zzzz").has_value());
}

REF_TEST(core, json_writer_is_compact_and_deterministic) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field("count", static_cast<std::uint64_t>(3));
  writer.field("enabled", true);
  writer.key("items");
  writer.begin_array();
  writer.number(static_cast<std::uint64_t>(1));
  writer.number(static_cast<std::uint64_t>(2));
  writer.end_array();
  writer.end_object();
  const std::string expected = "{\"status\": \"OK\",\"count\": 3,\"enabled\": true,\"items\": [1,2]}";
  REF_CHECK_EQ(writer.str(), expected);
  for (const char c : writer.str()) {
    REF_CHECK(c != '\n');
  }
}

REF_TEST(core, bounded_queue_rejects_explicitly_and_preserves_gaps) {
  BoundedQueue<int> queue(2);
  REF_CHECK(queue.offer(1).has_value());
  REF_CHECK(queue.offer(2).has_value());
  REF_CHECK(!queue.offer(3).has_value());  // explicit rejection
  REF_CHECK_EQ(queue.rejected(), 1ull);
  REF_CHECK_EQ(queue.sequences_issued(), 3ull);
  auto first = queue.wait_pop();
  REF_CHECK(first.has_value());
  REF_CHECK_EQ(first->first, 1ull);
  REF_CHECK_EQ(first->second, 1);
  queue.close();
  auto remaining = queue.wait_pop();
  REF_CHECK(remaining.has_value());
  auto drained = queue.wait_pop();
  REF_CHECK(!drained.has_value());
}

REF_TEST(compat, permissions_are_derived_from_aspects) {
  CompatibilityEdge edge = make_edge(1, 2, "mixed-version");
  REF_CHECK(has_permission(edge.permissions, CompatPermission::ControlChannel));
  REF_CHECK(has_permission(edge.permissions, CompatPermission::CoexistDuringRollout));
  REF_CHECK(has_permission(edge.permissions, CompatPermission::MutatingMessages));
  REF_CHECK(!has_permission(edge.permissions, CompatPermission::WriteSharedState) == false);
  // Unknown aspects never grant permission.
  CompatibilityEdge unknown;
  unknown.from = RuntimeGeneration::from_raw(1);
  unknown.to = RuntimeGeneration::from_raw(2);
  unknown.generation = CompatibilityGeneration::first();
  unknown.permissions = derive_permissions(unknown);
  REF_CHECK_EQ(unknown.permissions, static_cast<CompatPermissions>(0));
  REF_CHECK(!has_permission(unknown.permissions, CompatPermission::ControlChannel));
}

REF_TEST(compat, mismatched_permissions_are_rejected) {
  CompatibilityEdge edge = make_edge(1, 2, "compatible");
  edge.permissions = 0;  // contradicts the aspects
  const Status status = validate_edge(edge);
  REF_CHECK_STATUS_CODE(status, ErrorCode::Corrupt);
  CompatibilityMatrix matrix;
  REF_CHECK_STATUS_CODE(matrix.upsert(edge), ErrorCode::Corrupt);
  REF_CHECK_EQ(matrix.edge_count(), 0u);
}

REF_TEST(compat, matrix_is_deterministic_and_bounded) {
  CompatibilityMatrix matrix;
  REF_CHECK_STATUS_OK(matrix.upsert(make_edge(1, 2, "compatible")));
  REF_CHECK_STATUS_OK(matrix.upsert(make_edge(1, 3, "compatible")));
  REF_CHECK_STATUS_OK(matrix.upsert(make_edge(3, 1, "compatible")));
  const auto neighbours = matrix.neighbours(RuntimeGeneration::from_raw(1));
  REF_CHECK_EQ(neighbours.size(), 2u);
  REF_CHECK_EQ(neighbours[0].raw(), 2ull);
  REF_CHECK_EQ(neighbours[1].raw(), 3ull);
  REF_CHECK(matrix.permits(RuntimeGeneration::from_raw(1), RuntimeGeneration::from_raw(2),
                           CompatPermission::ControlChannel));
  REF_CHECK(!matrix.permits(RuntimeGeneration::from_raw(2), RuntimeGeneration::from_raw(1),
                            CompatPermission::ControlChannel));
  REF_CHECK_STATUS_OK(matrix.validate());
  REF_CHECK(matrix.find(RuntimeGeneration::from_raw(9), RuntimeGeneration::from_raw(1)) == nullptr);
}

REF_TEST(compat, blocking_outcomes_are_classified) {
  REF_CHECK(is_blocking_outcome(CompatOutcome::Unknown));
  REF_CHECK(is_blocking_outcome(CompatOutcome::StaleEvidence));
  REF_CHECK(is_blocking_outcome(CompatOutcome::Unsupported));
  REF_CHECK(is_blocking_outcome(CompatOutcome::IncompatibleProtocol));
  REF_CHECK(!is_blocking_outcome(CompatOutcome::ReadCompatible));
  REF_CHECK(!is_blocking_outcome(CompatOutcome::RollbackCompatible));
}

REF_TEST(protocol, descriptor_validation_rejects_impossible_declarations) {
  ProtocolDescriptor descriptor = build_wire_protocol(ProtocolGeneration::from_raw(1));
  REF_CHECK_STATUS_OK(descriptor.validate());
  ProtocolDescriptor bad_floor = descriptor;
  bad_floor.minimum_safety_generation = ProtocolGeneration::from_raw(9);
  REF_CHECK_STATUS_CODE(bad_floor.validate(), ErrorCode::InvalidArgument);
  ProtocolDescriptor no_messages = descriptor;
  no_messages.messages.clear();
  REF_CHECK_STATUS_CODE(no_messages.validate(), ErrorCode::InvalidArgument);
  ProtocolDescriptor no_evidence = descriptor;
  no_evidence.provenance = EvidenceClass::Unknown;
  REF_CHECK_STATUS_CODE(no_evidence.validate(), ErrorCode::InvalidArgument);
}

REF_TEST(protocol, negotiation_prefers_highest_common_without_downgrading_below_floor) {
  ProtocolRegistry registry = make_protocol_registry();
  ProtocolNegotiator negotiator(&registry);
  NegotiationRequest request;
  request.protocol = ProtocolId::from_valid("ref-wire");
  (void)request.local_supported.add(ProtocolGeneration::from_raw(1));
  (void)request.local_supported.add(ProtocolGeneration::from_raw(2));
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(1));
  request.required_minimum = ProtocolGeneration::from_raw(1);
  // The peer only speaks generation 1, so generation 1 is the highest common
  // generation: that is not a downgrade, it is the only agreement available.
  const NegotiationResult limited_peer = negotiator.negotiate(request);
  REF_CHECK(limited_peer.is_ok());
  REF_CHECK_EQ(limited_peer.chosen.raw(), 1ull);
  REF_CHECK(!limited_peer.downgraded);
  REF_CHECK_EQ(limited_peer.operation_class, OperationClass::FullMutation);

  // A pinned preferred generation below the highest common generation is a
  // real downgrade, and it reduces the operation class.
  request.preferred = ProtocolGeneration::from_raw(1);
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(2));
  const NegotiationResult downgraded = negotiator.negotiate(request);
  REF_CHECK(downgraded.is_ok());
  REF_CHECK_EQ(downgraded.chosen.raw(), 1ull);
  REF_CHECK(downgraded.downgraded);
  REF_CHECK_OUTCOME(downgraded.outcome, CompatOutcome::CompatibleAfterProtocolNegotiation);
  REF_CHECK_EQ(downgraded.operation_class, OperationClass::RestrictedMutation);
  request.preferred = ProtocolGeneration::unset();

  // A peer that can only speak generation 1 must be refused when policy
  // requires generation 2: the fabric never silently drops below its floor.
  request.peer_supported = ProtocolGenerationSet{};
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(1));
  request.required_minimum = ProtocolGeneration::from_raw(2);
  const NegotiationResult below_floor = negotiator.negotiate(request);
  REF_CHECK(below_floor.status.is_failure());
  REF_CHECK_OUTCOME(below_floor.outcome, CompatOutcome::IncompatibleProtocol);

  request.peer_supported = ProtocolGenerationSet{};
  (void)request.peer_supported.add(ProtocolGeneration::from_raw(2));
  request.required_minimum = ProtocolGeneration::from_raw(1);
  const NegotiationResult full = negotiator.negotiate(request);
  REF_CHECK(full.is_ok());
  REF_CHECK_EQ(full.chosen.raw(), 2ull);
  REF_CHECK_EQ(full.operation_class, OperationClass::FullMutation);
}

REF_TEST(protocol, message_availability_is_generation_bound) {
  const ProtocolDescriptor v1 = build_wire_protocol(ProtocolGeneration::from_raw(1));
  const ProtocolDescriptor v2 = build_wire_protocol(ProtocolGeneration::from_raw(2));
  REF_CHECK(v1.supports_message(MessageTypeId::from_valid("QUERY_STATE")));
  REF_CHECK(!v1.supports_message(MessageTypeId::from_valid("REQUEST_STATE_TRANSFORM")));
  REF_CHECK(v2.supports_message(MessageTypeId::from_valid("REQUEST_STATE_TRANSFORM")));
  REF_CHECK(v1.supports_mutating(MessageTypeId::from_valid("PUBLISH_STAGE")));
  REF_CHECK(!v1.supports_mutating(MessageTypeId::from_valid("QUERY_STATE")));
  REF_CHECK_EQ(message_introduced_in(MessageType::RequestStateTransform).raw(), 2ull);
  REF_CHECK_EQ(message_introduced_in(MessageType::QueryState).raw(), 1ull);
}

REF_TEST(schema, read_and_write_compatibility_are_distinct) {
  SchemaRegistry registry = make_schema_registry();
  const SchemaId id = SchemaId::from_valid("component-state");
  const SchemaCompatResult v2_reads_v1 =
      registry.read_compatibility(id, SchemaGeneration::from_raw(2), SchemaGeneration::from_raw(1));
  REF_CHECK_OUTCOME(v2_reads_v1.outcome, CompatOutcome::ReadCompatible);
  const SchemaCompatResult v1_reads_v2 =
      registry.read_compatibility(id, SchemaGeneration::from_raw(1), SchemaGeneration::from_raw(2));
  REF_CHECK_OUTCOME(v1_reads_v2.outcome, CompatOutcome::IncompatibleSchema);
  // A v2 writer can still emit the v1 form; a v3 writer cannot.
  const SchemaCompatResult v2_writes_v1_consumer =
      registry.write_compatibility(id, SchemaGeneration::from_raw(2), SchemaGeneration::from_raw(1));
  REF_CHECK(v2_writes_v1_consumer.outcome == CompatOutcome::WriteCompatible ||
            v2_writes_v1_consumer.outcome == CompatOutcome::MixedVersionCompatible);
  const SchemaCompatResult v3_writes_v1_consumer =
      registry.write_compatibility(id, SchemaGeneration::from_raw(3), SchemaGeneration::from_raw(1));
  REF_CHECK_OUTCOME(v3_writes_v1_consumer.outcome, CompatOutcome::IncompatibleSchema);
  REF_CHECK(registry.has_reverse_migration(id, SchemaGeneration::from_raw(2)));
  REF_CHECK(!registry.has_reverse_migration(id, SchemaGeneration::from_raw(3)));
}

REF_TEST(schema, contradictory_descriptors_are_rejected) {
  SchemaRegistry registry = make_schema_registry();
  SchemaDescriptor base = *registry.find(SchemaId::from_valid("component-state"),
                                         SchemaGeneration::from_raw(3));
  SchemaDescriptor reversible_irreversible = base;
  reversible_irreversible.reverse_migration_to = SchemaGeneration::from_raw(2);
  REF_CHECK_STATUS_CODE(reversible_irreversible.validate(), ErrorCode::InvalidArgument);
  SchemaDescriptor unreadable_self = base;
  unreadable_self.readable_formats = SchemaGenerationRange::single(SchemaGeneration::from_raw(9));
  REF_CHECK_STATUS_CODE(unreadable_self.validate(), ErrorCode::InvalidArgument);
  SchemaDescriptor no_canonicalization = base;
  no_canonicalization.canonicalization = CanonicalizationRule::None;
  REF_CHECK_STATUS_CODE(no_canonicalization.validate(), ErrorCode::InvalidArgument);
}