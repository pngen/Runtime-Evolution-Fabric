#include "ref/store.hpp"

#include <algorithm>
#include <array>

namespace ref {
namespace {

// ---------------------------------------------------------------------------
// Primitive codec helpers
// ---------------------------------------------------------------------------
template <class Tag>
void write_generations(ByteWriter& writer, const GenerationSet<Tag>& set) {
  writer.u8(set.size());
  for (std::uint8_t i = 0; i < set.size(); ++i) writer.generation(set.at(i));
}

template <class Tag>
Status read_generations(ByteReader& reader, GenerationSet<Tag>& set, const char* what) {
  const std::uint8_t count = reader.u8();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (count > kMaxGenerationsPerSet) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint8_t i = 0; i < count; ++i) {
    const auto value = reader.generation<Tag>();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    if (!set.add(value)) return Status::failure(ErrorCode::Corrupt, what);
  }
  if (set.size() != count) return Status::failure(ErrorCode::Corrupt, what);
  return Status::ok();
}

template <class Tag>
void write_range(ByteWriter& writer, const GenerationRange<Tag>& range) {
  writer.generation(range.min);
  writer.generation(range.max);
}

template <class Tag>
Status read_range(ByteReader& reader, GenerationRange<Tag>& range, const char* what) {
  range.min = reader.generation<Tag>();
  range.max = reader.generation<Tag>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (range.min.is_set() != range.max.is_set()) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  if (range.is_set() && !range.is_valid()) return Status::failure(ErrorCode::Corrupt, what);
  return Status::ok();
}

void write_version(ByteWriter& writer, const RuntimeVersionId& version) {
  writer.u16(version.number.major);
  writer.u16(version.number.minor);
  writer.u16(version.number.patch);
  writer.ident(version.artifact.build);
  writer.digest(version.artifact.content_hash);
}

Status read_version(ByteReader& reader, RuntimeVersionId& version, const char* what) {
  version.number.major = reader.u16();
  version.number.minor = reader.u16();
  version.number.patch = reader.u16();
  version.artifact.build = reader.ident<BuildIdTag, 63>();
  version.artifact.content_hash = reader.digest();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_evidence_ref(ByteWriter& writer, const EvidenceRef& evidence) {
  writer.u8(static_cast<std::uint8_t>(evidence.provenance));
  writer.generation(evidence.generation);
  writer.ident(evidence.source);
  writer.text(evidence.detail.view(), DetailText::capacity);
}

Status read_evidence_ref(ByteReader& reader, EvidenceRef& evidence, const char* what) {
  const std::uint8_t provenance = reader.u8();
  if (reader.failed() || provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  evidence.provenance = static_cast<EvidenceClass>(provenance);
  evidence.generation = reader.generation<EvidenceGenerationTag>();
  evidence.source = reader.ident<EvidenceSourceIdTag, 63>();
  evidence.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_required_evidence(ByteWriter& writer, const RequiredEvidence& required) {
  writer.u8(static_cast<std::uint8_t>(required.minimum_provenance));
  writer.u64(required.max_age_generations);
  writer.text(required.description.view(), DetailText::capacity);
}

Status read_required_evidence(ByteReader& reader, RequiredEvidence& required, const char* what) {
  const std::uint8_t provenance = reader.u8();
  required.max_age_generations = reader.u64();
  required.description = reader.text<DetailText::capacity>();
  if (reader.failed() || provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  required.minimum_provenance = static_cast<EvidenceClass>(provenance);
  return Status::ok();
}

void write_feature_support(ByteWriter& writer, const FeatureSupport& feature) {
  writer.ident(feature.feature);
  writer.generation(feature.generation);
  writer.boolean(feature.can_publish);
  writer.boolean(feature.can_consume);
}

Status read_feature_support(ByteReader& reader, FeatureSupport& feature, const char* what) {
  feature.feature = reader.ident<FeatureGateIdTag, 63>();
  feature.generation = reader.generation<FeatureGateGenerationTag>();
  feature.can_publish = reader.boolean();
  feature.can_consume = reader.boolean();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_component_version(ByteWriter& writer, const RuntimeComponentVersion& version) {
  writer.ident(version.component);
  write_version(writer, version.version);
  writer.generation(version.generation);
  write_generations(writer, version.protocols.supported);
  write_generations(writer, version.protocols.readable);
  write_generations(writer, version.protocols.writable);
  writer.generation(version.protocols.minimum_safety);
  write_generations(writer, version.schemas.supported);
  write_generations(writer, version.schemas.readable);
  write_generations(writer, version.schemas.writable);
  write_generations(writer, version.schemas.readable_formats);
  write_generations(writer, version.schemas.writable_formats);
  writer.u32(static_cast<std::uint32_t>(version.migrations.size()));
  for (const auto& migration : version.migrations) {
    writer.ident(migration.id);
    writer.generation(migration.generation);
    writer.ident(migration.schema);
    writer.generation(migration.source);
    writer.generation(migration.target);
    writer.boolean(migration.reversible);
    writer.boolean(migration.preserves_rollback_metadata);
  }
  writer.boolean(version.rollback.supported);
  write_range(writer, version.rollback.target_window);
  writer.boolean(version.rollback.requires_state_restore);
  writer.boolean(version.rollback.reverse_migration_available);
  writer.u32(static_cast<std::uint32_t>(version.features.size()));
  for (const auto& feature : version.features) write_feature_support(writer, feature);
  writer.generation(version.capability_generation);
  writer.u32(static_cast<std::uint32_t>(version.peers.size()));
  for (const auto& peer : version.peers) {
    writer.ident(peer.peer);
    write_range(writer, peer.generations);
    writer.boolean(peer.required);
  }
  write_range(writer, version.compatible_generations);
  writer.u8(static_cast<std::uint8_t>(version.lifecycle));
  writer.u8(static_cast<std::uint8_t>(version.provenance));
  writer.generation(version.evidence);
  writer.generation(version.epoch);
  writer.text(version.notes.view(), NoteText::capacity);
}

Status read_component_version(ByteReader& reader, RuntimeComponentVersion& version) {
  const char* what = "durable runtime component record is malformed";
  version.component = reader.ident<RuntimeComponentIdTag, 63>();
  Status status = read_version(reader, version.version, what);
  if (status.is_failure()) return status;
  version.generation = reader.generation<RuntimeGenerationTag>();
  status = read_generations(reader, version.protocols.supported, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.protocols.readable, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.protocols.writable, what);
  if (status.is_failure()) return status;
  version.protocols.minimum_safety = reader.generation<ProtocolGenerationTag>();
  status = read_generations(reader, version.schemas.supported, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.schemas.readable, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.schemas.writable, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.schemas.readable_formats, what);
  if (status.is_failure()) return status;
  status = read_generations(reader, version.schemas.writable_formats, what);
  if (status.is_failure()) return status;
  const std::uint32_t migration_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (migration_count > limits::kMessageFields) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < migration_count; ++i) {
    MigrationCapability migration;
    migration.id = reader.ident<MigrationIdTag, 63>();
    migration.generation = reader.generation<MigrationGenerationTag>();
    migration.schema = reader.ident<SchemaIdTag, 63>();
    migration.source = reader.generation<SchemaGenerationTag>();
    migration.target = reader.generation<SchemaGenerationTag>();
    migration.reversible = reader.boolean();
    migration.preserves_rollback_metadata = reader.boolean();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    version.migrations.push_back(migration);
  }
  version.rollback.supported = reader.boolean();
  status = read_range(reader, version.rollback.target_window, what);
  if (status.is_failure()) return status;
  version.rollback.requires_state_restore = reader.boolean();
  version.rollback.reverse_migration_available = reader.boolean();
  const std::uint32_t feature_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (feature_count > limits::kFeatureGates) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < feature_count; ++i) {
    FeatureSupport feature;
    status = read_feature_support(reader, feature, what);
    if (status.is_failure()) return status;
    version.features.push_back(feature);
  }
  version.capability_generation = reader.generation<CapabilityGenerationTag>();
  const std::uint32_t peer_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (peer_count > limits::kRuntimeComponents) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < peer_count; ++i) {
    PeerRequirement peer;
    peer.peer = reader.ident<RuntimeComponentIdTag, 63>();
    status = read_range(reader, peer.generations, what);
    if (status.is_failure()) return status;
    peer.required = reader.boolean();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    version.peers.push_back(peer);
  }
  status = read_range(reader, version.compatible_generations, what);
  if (status.is_failure()) return status;
  const std::uint8_t lifecycle = reader.u8();
  const std::uint8_t provenance = reader.u8();
  version.evidence = reader.generation<EvidenceGenerationTag>();
  version.epoch = reader.generation<EvolutionEpochTag>();
  version.notes = reader.text<NoteText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (lifecycle > static_cast<std::uint8_t>(LifecycleState::Failed)) {
    return Status::failure(ErrorCode::Corrupt, "durable runtime component has an impossible lifecycle");
  }
  if (provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  version.lifecycle = static_cast<LifecycleState>(lifecycle);
  version.provenance = static_cast<EvidenceClass>(provenance);
  return Status::ok();
}

void write_protocol(ByteWriter& writer, const ProtocolDescriptor& descriptor) {
  writer.ident(descriptor.id);
  writer.generation(descriptor.generation);
  writer.generation(descriptor.minimum_safety_generation);
  writer.u8(static_cast<std::uint8_t>(descriptor.unknown_fields));
  writer.u8(static_cast<std::uint8_t>(descriptor.unknown_messages));
  writer.u8(static_cast<std::uint8_t>(descriptor.provenance));
  writer.generation(descriptor.evidence);
  writer.text(descriptor.notes.view(), NoteText::capacity);
  writer.u32(static_cast<std::uint32_t>(descriptor.messages.size()));
  for (const auto& message : descriptor.messages) {
    writer.ident(message.id);
    writer.generation(message.introduced_in);
    writer.boolean(message.mutating);
    writer.boolean(message.state_replacing);
    writer.u32(static_cast<std::uint32_t>(message.fields.size()));
    for (const auto& field : message.fields) {
      writer.ident(field.id);
      writer.generation(field.introduced_in);
      writer.boolean(field.required);
    }
  }
}

Status read_protocol(ByteReader& reader, ProtocolDescriptor& descriptor) {
  const char* what = "durable protocol descriptor is malformed";
  descriptor.id = reader.ident<ProtocolIdTag, 63>();
  descriptor.generation = reader.generation<ProtocolGenerationTag>();
  descriptor.minimum_safety_generation = reader.generation<ProtocolGenerationTag>();
  const std::uint8_t unknown_fields = reader.u8();
  const std::uint8_t unknown_messages = reader.u8();
  const std::uint8_t provenance = reader.u8();
  descriptor.evidence = reader.generation<EvidenceGenerationTag>();
  descriptor.notes = reader.text<NoteText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (unknown_fields > static_cast<std::uint8_t>(UnknownFieldPolicy::Preserve) ||
      unknown_messages > static_cast<std::uint8_t>(UnknownMessagePolicy::Ignore) ||
      provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  descriptor.unknown_fields = static_cast<UnknownFieldPolicy>(unknown_fields);
  descriptor.unknown_messages = static_cast<UnknownMessagePolicy>(unknown_messages);
  descriptor.provenance = static_cast<EvidenceClass>(provenance);
  const std::uint32_t message_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (message_count > limits::kMessageTypes) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < message_count; ++i) {
    MessageTypeDescriptor message;
    message.id = reader.ident<MessageTypeIdTag, 63>();
    message.introduced_in = reader.generation<ProtocolGenerationTag>();
    message.mutating = reader.boolean();
    message.state_replacing = reader.boolean();
    const std::uint32_t field_count = reader.u32();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    if (field_count > limits::kMessageFields) return Status::failure(ErrorCode::Corrupt, what);
    for (std::uint32_t j = 0; j < field_count; ++j) {
      FieldDescriptor field;
      field.id = reader.ident<FieldIdTag, 63>();
      field.introduced_in = reader.generation<ProtocolGenerationTag>();
      field.required = reader.boolean();
      if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
      message.fields.push_back(field);
    }
    descriptor.messages.push_back(message);
  }
  return Status::ok();
}

void write_schema(ByteWriter& writer, const SchemaDescriptor& descriptor) {
  writer.ident(descriptor.id);
  writer.generation(descriptor.generation);
  write_range(writer, descriptor.readable_formats);
  write_range(writer, descriptor.writable_formats);
  writer.generation(descriptor.reverse_migration_to);
  writer.boolean(descriptor.requires_explicit_commit);
  writer.u8(static_cast<std::uint8_t>(descriptor.canonicalization));
  writer.u8(static_cast<std::uint8_t>(descriptor.integrity));
  writer.u8(static_cast<std::uint8_t>(descriptor.provenance));
  writer.generation(descriptor.evidence);
  writer.text(descriptor.notes.view(), NoteText::capacity);
  writer.u32(static_cast<std::uint32_t>(descriptor.fields.size()));
  for (const auto& field : descriptor.fields) {
    writer.ident(field.id);
    writer.u8(static_cast<std::uint8_t>(field.type));
    writer.generation(field.introduced_in);
    writer.generation(field.removed_in);
    writer.boolean(field.required);
    writer.boolean(field.irreversible);
    writer.text(field.required_default.view(), NoteText::capacity);
  }
}

Status read_schema(ByteReader& reader, SchemaDescriptor& descriptor) {
  const char* what = "durable schema descriptor is malformed";
  descriptor.id = reader.ident<SchemaIdTag, 63>();
  descriptor.generation = reader.generation<SchemaGenerationTag>();
  Status status = read_range(reader, descriptor.readable_formats, what);
  if (status.is_failure()) return status;
  status = read_range(reader, descriptor.writable_formats, what);
  if (status.is_failure()) return status;
  descriptor.reverse_migration_to = reader.generation<SchemaGenerationTag>();
  descriptor.requires_explicit_commit = reader.boolean();
  const std::uint8_t canonicalization = reader.u8();
  const std::uint8_t integrity = reader.u8();
  const std::uint8_t provenance = reader.u8();
  descriptor.evidence = reader.generation<EvidenceGenerationTag>();
  descriptor.notes = reader.text<NoteText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (canonicalization > static_cast<std::uint8_t>(CanonicalizationRule::FixedFieldOrderStableEncoding) ||
      integrity > static_cast<std::uint8_t>(IntegritySemantics::ChecksumAndSchemaBinding) ||
      provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  descriptor.canonicalization = static_cast<CanonicalizationRule>(canonicalization);
  descriptor.integrity = static_cast<IntegritySemantics>(integrity);
  descriptor.provenance = static_cast<EvidenceClass>(provenance);
  const std::uint32_t field_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (field_count > limits::kMessageFields) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < field_count; ++i) {
    SchemaField field;
    field.id = reader.ident<FieldIdTag, 63>();
    const std::uint8_t type = reader.u8();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    if (type > static_cast<std::uint8_t>(FieldType::Digest)) {
      return Status::failure(ErrorCode::Corrupt, "durable schema field type is unknown");
    }
    field.type = static_cast<FieldType>(type);
    field.introduced_in = reader.generation<SchemaGenerationTag>();
    field.removed_in = reader.generation<SchemaGenerationTag>();
    field.required = reader.boolean();
    field.irreversible = reader.boolean();
    field.required_default = reader.text<NoteText::capacity>();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    descriptor.fields.push_back(field);
  }
  return Status::ok();
}

void write_edge(ByteWriter& writer, const CompatibilityEdge& edge) {
  writer.generation(edge.from);
  writer.generation(edge.to);
  for (const auto& assessment : edge.aspects) {
    writer.u8(static_cast<std::uint8_t>(assessment.outcome));
    writer.u8(static_cast<std::uint8_t>(assessment.provenance));
    writer.generation(assessment.evidence);
    writer.text(assessment.detail.view(), DetailText::capacity);
  }
  writer.u16(edge.permissions);
  writer.generation(edge.generation);
  writer.generation(edge.evidence);
  writer.generation(edge.epoch);
  writer.boolean(edge.requires_feature_gate);
  writer.ident(edge.gate);
  writer.generation(edge.gate_generation);
  writer.boolean(edge.requires_protocol_downgrade);
  writer.generation(edge.downgrade_to);
  writer.boolean(edge.requires_state_translation);
  writer.ident(edge.migration);
  writer.generation(edge.migration_generation);
}

Status read_edge(ByteReader& reader, CompatibilityEdge& edge) {
  const char* what = "durable compatibility edge is malformed";
  edge.from = reader.generation<RuntimeGenerationTag>();
  edge.to = reader.generation<RuntimeGenerationTag>();
  for (std::size_t i = 0; i < kCompatAspectCount; ++i) {
    const std::uint8_t outcome = reader.u8();
    const std::uint8_t provenance = reader.u8();
    edge.aspects[i].evidence = reader.generation<EvidenceGenerationTag>();
    edge.aspects[i].detail = reader.text<DetailText::capacity>();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    if (outcome >= kCompatOutcomeCount || provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
      return Status::failure(ErrorCode::Corrupt, what);
    }
    edge.aspects[i].outcome = static_cast<CompatOutcome>(outcome);
    edge.aspects[i].provenance = static_cast<EvidenceClass>(provenance);
  }
  edge.permissions = reader.u16();
  edge.generation = reader.generation<CompatibilityGenerationTag>();
  edge.evidence = reader.generation<EvidenceGenerationTag>();
  edge.epoch = reader.generation<EvolutionEpochTag>();
  edge.requires_feature_gate = reader.boolean();
  edge.gate = reader.ident<FeatureGateIdTag, 63>();
  edge.gate_generation = reader.generation<FeatureGateGenerationTag>();
  edge.requires_protocol_downgrade = reader.boolean();
  edge.downgrade_to = reader.generation<ProtocolGenerationTag>();
  edge.requires_state_translation = reader.boolean();
  edge.migration = reader.ident<MigrationIdTag, 63>();
  edge.migration_generation = reader.generation<MigrationGenerationTag>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_gate(ByteWriter& writer, const FeatureGate& gate) {
  writer.ident(gate.id);
  writer.generation(gate.generation);
  writer.generation(gate.min_runtime_generation);
  writer.generation(gate.min_protocol_generation);
  writer.generation(gate.min_schema_generation);
  writer.u8(static_cast<std::uint8_t>(gate.required_peer_aspect));
  writer.u8(static_cast<std::uint8_t>(gate.required_peer_outcome));
  writer.ident(gate.scope);
  writer.boolean(gate.enabled);
  writer.ident(gate.enabled_by);
  writer.generation(gate.enabled_at);
  writer.u8(static_cast<std::uint8_t>(gate.provenance));
  writer.generation(gate.evidence);
  writer.text(gate.notes.view(), NoteText::capacity);
}

Status read_gate(ByteReader& reader, FeatureGate& gate) {
  const char* what = "durable feature gate is malformed";
  gate.id = reader.ident<FeatureGateIdTag, 63>();
  gate.generation = reader.generation<FeatureGateGenerationTag>();
  gate.min_runtime_generation = reader.generation<RuntimeGenerationTag>();
  gate.min_protocol_generation = reader.generation<ProtocolGenerationTag>();
  gate.min_schema_generation = reader.generation<SchemaGenerationTag>();
  const std::uint8_t aspect = reader.u8();
  const std::uint8_t outcome = reader.u8();
  gate.scope = reader.ident<CohortIdTag, 63>();
  gate.enabled = reader.boolean();
  gate.enabled_by = reader.ident<EvolutionPlanIdTag, 63>();
  gate.enabled_at = reader.generation<StageGenerationTag>();
  const std::uint8_t provenance = reader.u8();
  gate.evidence = reader.generation<EvidenceGenerationTag>();
  gate.notes = reader.text<NoteText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (aspect >= kCompatAspectCount || outcome >= kCompatOutcomeCount ||
      provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  gate.required_peer_aspect = static_cast<CompatAspect>(aspect);
  gate.required_peer_outcome = static_cast<CompatOutcome>(outcome);
  gate.provenance = static_cast<EvidenceClass>(provenance);
  return Status::ok();
}

void write_stage_record(ByteWriter& writer, const PlanStageRecord& record) {
  writer.ident(record.stage_id);
  writer.u8(static_cast<std::uint8_t>(record.stage));
  writer.generation(record.generation);
  writer.generation(record.epoch);
  writer.generation(record.coordinator_epoch);
  writer.generation(record.evidence);
  writer.boolean(record.completed);
  writer.text(record.note.view(), NoteText::capacity);
}

Status read_stage_record(ByteReader& reader, PlanStageRecord& record) {
  const char* what = "durable stage record is malformed";
  record.stage_id = reader.ident<RolloutStageIdTag, 63>();
  const std::uint8_t stage = reader.u8();
  record.generation = reader.generation<StageGenerationTag>();
  record.epoch = reader.generation<EvolutionEpochTag>();
  record.coordinator_epoch = reader.generation<CoordinatorEpochTag>();
  record.evidence = reader.generation<EvidenceGenerationTag>();
  record.completed = reader.boolean();
  record.note = reader.text<NoteText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (stage >= kRolloutStageCount) return Status::failure(ErrorCode::Corrupt, what);
  record.stage = static_cast<RolloutStage>(stage);
  return Status::ok();
}

void write_plan(ByteWriter& writer, const EvolutionPlan& plan) {
  writer.ident(plan.id);
  writer.generation(plan.generation);
  writer.ident(plan.component);
  writer.generation(plan.current_generation);
  writer.generation(plan.candidate_generation);
  writer.generation(plan.matrix_generation);
  writer.generation(plan.protocol_generation);
  writer.generation(plan.schema_generation);
  writer.generation(plan.gate_generation);
  writer.ident(plan.migration);
  writer.generation(plan.migration_generation);
  writer.generation(plan.migration_source);
  writer.generation(plan.migration_target);
  writer.generation(plan.rollback_target);
  writer.generation(plan.rollback_generation);
  writer.boolean(plan.rollback_barrier_crossed);
  writer.u32(static_cast<std::uint32_t>(plan.required_evidence.size()));
  for (const auto& required : plan.required_evidence) write_required_evidence(writer, required);
  writer.generation(plan.coordinator_epoch);
  writer.generation(plan.epoch);
  writer.u8(static_cast<std::uint8_t>(plan.stage));
  writer.generation(plan.stage_generation);
  writer.u32(static_cast<std::uint32_t>(plan.cohorts.size()));
  for (const auto& cohort : plan.cohorts) writer.ident(cohort);
  writer.ident(plan.canary_cohort);
  writer.boolean(plan.new_writer_enabled);
  writer.boolean(plan.policy.require_canary);
  writer.boolean(plan.policy.require_mixed_version_cohort);
  writer.boolean(plan.policy.require_migration_barrier);
  writer.boolean(plan.policy.require_new_writer_barrier);
  writer.boolean(plan.policy.require_old_writer_drain);
  writer.boolean(plan.policy.require_rollback_proof);
  writer.generation(plan.policy.generation);
  writer.u32(static_cast<std::uint32_t>(plan.history.size()));
  for (const auto& record : plan.history) write_stage_record(writer, record);
  writer.boolean(plan.superseded);
  writer.generation(plan.evidence);
  writer.text(plan.detail.view(), DetailText::capacity);
}

Status read_plan(ByteReader& reader, EvolutionPlan& plan) {
  const char* what = "durable evolution plan is malformed";
  plan.id = reader.ident<EvolutionPlanIdTag, 63>();
  plan.generation = reader.generation<EvolutionPlanGenerationTag>();
  plan.component = reader.ident<RuntimeComponentIdTag, 63>();
  plan.current_generation = reader.generation<RuntimeGenerationTag>();
  plan.candidate_generation = reader.generation<RuntimeGenerationTag>();
  plan.matrix_generation = reader.generation<CompatibilityGenerationTag>();
  plan.protocol_generation = reader.generation<ProtocolGenerationTag>();
  plan.schema_generation = reader.generation<SchemaGenerationTag>();
  plan.gate_generation = reader.generation<FeatureGateGenerationTag>();
  plan.migration = reader.ident<MigrationIdTag, 63>();
  plan.migration_generation = reader.generation<MigrationGenerationTag>();
  plan.migration_source = reader.generation<SchemaGenerationTag>();
  plan.migration_target = reader.generation<SchemaGenerationTag>();
  plan.rollback_target = reader.generation<RuntimeGenerationTag>();
  plan.rollback_generation = reader.generation<RollbackGenerationTag>();
  plan.rollback_barrier_crossed = reader.boolean();
  const std::uint32_t evidence_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (evidence_count > limits::kExplanationLines) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < evidence_count; ++i) {
    RequiredEvidence required;
    const Status status = read_required_evidence(reader, required, what);
    if (status.is_failure()) return status;
    plan.required_evidence.push_back(required);
  }
  plan.coordinator_epoch = reader.generation<CoordinatorEpochTag>();
  plan.epoch = reader.generation<EvolutionEpochTag>();
  const std::uint8_t stage = reader.u8();
  plan.stage_generation = reader.generation<StageGenerationTag>();
  const std::uint32_t cohort_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (stage >= kRolloutStageCount) return Status::failure(ErrorCode::Corrupt, what);
  if (cohort_count > limits::kCohorts) return Status::failure(ErrorCode::Corrupt, what);
  plan.stage = static_cast<RolloutStage>(stage);
  for (std::uint32_t i = 0; i < cohort_count; ++i) {
    const CohortId cohort = reader.ident<CohortIdTag, 63>();
    if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
    plan.cohorts.push_back(cohort);
  }
  plan.canary_cohort = reader.ident<CohortIdTag, 63>();
  plan.new_writer_enabled = reader.boolean();
  plan.policy.require_canary = reader.boolean();
  plan.policy.require_mixed_version_cohort = reader.boolean();
  plan.policy.require_migration_barrier = reader.boolean();
  plan.policy.require_new_writer_barrier = reader.boolean();
  plan.policy.require_old_writer_drain = reader.boolean();
  plan.policy.require_rollback_proof = reader.boolean();
  plan.policy.generation = reader.generation<PolicyGenerationTag>();
  const std::uint32_t history_count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (history_count > limits::kStageHistory) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < history_count; ++i) {
    PlanStageRecord record;
    const Status status = read_stage_record(reader, record);
    if (status.is_failure()) return status;
    plan.history.push_back(record);
  }
  plan.superseded = reader.boolean();
  plan.evidence = reader.generation<EvidenceGenerationTag>();
  plan.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_migration_record(ByteWriter& writer, const MigrationRecord& record) {
  writer.ident(record.id);
  writer.generation(record.generation);
  writer.ident(record.schema);
  writer.generation(record.source);
  writer.generation(record.target);
  writer.generation(record.runtime_generation);
  writer.generation(record.epoch);
  writer.generation(record.policy);
  writer.ident(record.plan);
  writer.digest(record.source_digest);
  writer.digest(record.target_digest);
  writer.boolean(record.irreversible);
  writer.boolean(record.rollback_metadata_preserved);
  writer.boolean(record.rollback_available);
  writer.u8(static_cast<std::uint8_t>(record.outcome));
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.generation(record.recorded_at);
  writer.text(record.detail.view(), DetailText::capacity);
}

Status read_migration_record(ByteReader& reader, MigrationRecord& record) {
  const char* what = "durable migration record is malformed";
  record.id = reader.ident<MigrationIdTag, 63>();
  record.generation = reader.generation<MigrationGenerationTag>();
  record.schema = reader.ident<SchemaIdTag, 63>();
  record.source = reader.generation<SchemaGenerationTag>();
  record.target = reader.generation<SchemaGenerationTag>();
  record.runtime_generation = reader.generation<RuntimeGenerationTag>();
  record.epoch = reader.generation<EvolutionEpochTag>();
  record.policy = reader.generation<PolicyGenerationTag>();
  record.plan = reader.ident<EvolutionPlanIdTag, 63>();
  record.source_digest = reader.digest();
  record.target_digest = reader.digest();
  record.irreversible = reader.boolean();
  record.rollback_metadata_preserved = reader.boolean();
  record.rollback_available = reader.boolean();
  const std::uint8_t outcome = reader.u8();
  const std::uint8_t provenance = reader.u8();
  record.recorded_at = reader.generation<StageGenerationTag>();
  record.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (outcome >= kMigrationOutcomeCount || provenance > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return Status::failure(ErrorCode::Corrupt, what);
  }
  record.outcome = static_cast<MigrationOutcome>(outcome);
  record.provenance = static_cast<EvidenceClass>(provenance);
  return Status::ok();
}

void write_rollback_record(ByteWriter& writer, const RollbackRecord& record) {
  writer.ident(record.id);
  writer.generation(record.generation);
  writer.ident(record.component);
  writer.generation(record.from_generation);
  writer.generation(record.to_generation);
  writer.ident(record.plan);
  writer.u8(static_cast<std::uint8_t>(record.outcome));
  writer.generation(record.epoch);
  writer.generation(record.coordinator_epoch);
  writer.generation(record.recorded_at);
  writer.text(record.detail.view(), DetailText::capacity);
}

Status read_rollback_record(ByteReader& reader, RollbackRecord& record) {
  const char* what = "durable rollback record is malformed";
  record.id = reader.ident<RollbackIdTag, 63>();
  record.generation = reader.generation<RollbackGenerationTag>();
  record.component = reader.ident<RuntimeComponentIdTag, 63>();
  record.from_generation = reader.generation<RuntimeGenerationTag>();
  record.to_generation = reader.generation<RuntimeGenerationTag>();
  record.plan = reader.ident<EvolutionPlanIdTag, 63>();
  const std::uint8_t outcome = reader.u8();
  record.epoch = reader.generation<EvolutionEpochTag>();
  record.coordinator_epoch = reader.generation<CoordinatorEpochTag>();
  record.recorded_at = reader.generation<StageGenerationTag>();
  record.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (outcome >= kRollbackOutcomeCount) return Status::failure(ErrorCode::Corrupt, what);
  record.outcome = static_cast<RollbackOutcome>(outcome);
  return Status::ok();
}

void write_retirement(ByteWriter& writer, const RetirementRecord& record) {
  writer.ident(record.component);
  writer.generation(record.generation);
  writer.generation(record.recorded_at);
  writer.generation(record.epoch);
  writer.generation(record.coordinator_epoch);
  writer.boolean(record.checkpoints_retained);
  writer.text(record.detail.view(), DetailText::capacity);
}

Status read_retirement(ByteReader& reader, RetirementRecord& record) {
  const char* what = "durable retirement record is malformed";
  record.component = reader.ident<RuntimeComponentIdTag, 63>();
  record.generation = reader.generation<RuntimeGenerationTag>();
  record.recorded_at = reader.generation<StageGenerationTag>();
  record.epoch = reader.generation<EvolutionEpochTag>();
  record.coordinator_epoch = reader.generation<CoordinatorEpochTag>();
  record.checkpoints_retained = reader.boolean();
  record.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_fenced_boot(ByteWriter& writer, const FencedBoot& record) {
  writer.ident(record.worker);
  writer.generation(record.boot);
  writer.ident(record.component);
  writer.generation(record.generation);
  writer.text(record.reason.view(), NoteText::capacity);
  writer.generation(record.fenced_at);
}

Status read_fenced_boot(ByteReader& reader, FencedBoot& record) {
  const char* what = "durable fenced boot record is malformed";
  record.worker = reader.ident<WorkerIdTag, 63>();
  record.boot = reader.generation<WorkerBootIdTag>();
  record.component = reader.ident<RuntimeComponentIdTag, 63>();
  record.generation = reader.generation<RuntimeGenerationTag>();
  record.reason = reader.text<NoteText::capacity>();
  record.fenced_at = reader.generation<StageGenerationTag>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

void write_worker_record(ByteWriter& writer, const WorkerRecord& record) {
  writer.ident(record.worker);
  writer.ident(record.component);
  writer.generation(record.generation);
  writer.generation(record.last_boot);
  writer.boolean(record.fenced);
  writer.boolean(record.drained);
  writer.generation(record.evidence);
  writer.text(record.detail.view(), DetailText::capacity);
}

Status read_worker_record(ByteReader& reader, WorkerRecord& record) {
  const char* what = "durable worker record is malformed";
  record.worker = reader.ident<WorkerIdTag, 63>();
  record.component = reader.ident<RuntimeComponentIdTag, 63>();
  record.generation = reader.generation<RuntimeGenerationTag>();
  record.last_boot = reader.generation<WorkerBootIdTag>();
  record.fenced = reader.boolean();
  record.drained = reader.boolean();
  record.evidence = reader.generation<EvidenceGenerationTag>();
  record.detail = reader.text<DetailText::capacity>();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  return Status::ok();
}

// Cross-section consistency that no single record can check on its own.
Status validate_cross_section(const DurableState& state) {
  std::map<std::pair<RuntimeComponentId, std::uint64_t>, bool> retired;
  for (const auto& [key, record] : state.retirements) {
    (void)record;
    retired[key] = true;
  }
  for (const auto& version : state.components.all_versions()) {
    const auto key = std::make_pair(version->component, version->generation.raw());
    if (retired.find(key) != retired.end() && is_authoritative_lifecycle(version->lifecycle)) {
      return Status::failure(ErrorCode::Corrupt, "durable state marks a retired generation as current");
    }
  }
  for (const auto& [id, plan] : state.plans) {
    if (id != plan.id) return Status::failure(ErrorCode::Corrupt, "plan key does not match its id");
    const Status status = plan.validate();
    if (status.is_failure()) return status;
    if (plan.rollback_barrier_crossed) {
      for (const auto& record : state.migrations) {
        if (record.plan != plan.id) continue;
        if (record.rollback_available) {
          return Status::failure(ErrorCode::Corrupt,
                                 "durable state contradicts itself: barrier crossed but rollback available");
        }
      }
    }
  }
  for (const auto& record : state.migrations) {
    if (record.source == record.target) {
      return Status::failure(ErrorCode::Corrupt, "migration record has identical source and target");
    }
    if (!record.source.is_set() || !record.target.is_set()) {
      return Status::failure(ErrorCode::Corrupt, "migration record has an unset generation");
    }
    if (record.irreversible && record.rollback_available) {
      return Status::failure(ErrorCode::Corrupt, "irreversible migration claims rollback availability");
    }
  }
  for (const auto& record : state.rollbacks) {
    if (!record.from_generation.is_set() || !record.to_generation.is_set()) {
      return Status::failure(ErrorCode::Corrupt, "rollback record has an unset generation");
    }
    if (record.from_generation == record.to_generation) {
      return Status::failure(ErrorCode::Corrupt, "rollback record rolls back to the same generation");
    }
  }
  return Status::ok();
}

}  // namespace

bool DurableState::is_retired(const RuntimeComponentId& component, RuntimeGeneration generation) const {
  return retirements.find(std::make_pair(component, generation.raw())) != retirements.end();
}

bool DurableState::is_boot_fenced(const WorkerId& worker, WorkerBootId boot) const {
  return fenced_boots.find(std::make_pair(worker, boot.raw())) != fenced_boots.end();
}

Status DurableState::validate() const {
  if (format_version != kEvolutionStateFormatVersion) {
    return Status::failure(ErrorCode::Unsupported, "durable state format version is not supported");
  }
  Status status = components.version_count() > limits::kComponentVersions
                      ? Status::failure(ErrorCode::Corrupt, "component count exceeds the bound")
                      : Status::ok();
  if (status.is_failure()) return status;
  status = matrix.validate();
  if (status.is_failure()) return status;
  status = protocols.validate();
  if (status.is_failure()) return status;
  status = schemas.validate();
  if (status.is_failure()) return status;
  status = gates.validate();
  if (status.is_failure()) return status;
  if (plans.size() > limits::kPlanHistory) {
    return Status::failure(ErrorCode::Corrupt, "plan count exceeds the bound");
  }
  if (migrations.size() > limits::kMigrationRecords) {
    return Status::failure(ErrorCode::Corrupt, "migration history exceeds the bound");
  }
  if (rollbacks.size() > limits::kRollbackRecords) {
    return Status::failure(ErrorCode::Corrupt, "rollback history exceeds the bound");
  }
  if (fenced_boots.size() > limits::kFencedBoots) {
    return Status::failure(ErrorCode::Corrupt, "fenced boot table exceeds the bound");
  }
  if (workers.size() > limits::kWorkers) {
    return Status::failure(ErrorCode::Corrupt, "worker table exceeds the bound");
  }
  if (matrix.generation() != matrix_generation) {
    return Status::failure(ErrorCode::Corrupt, "matrix generation disagrees with the compatibility matrix");
  }
  if (gates.generation() != gate_generation) {
    return Status::failure(ErrorCode::Corrupt, "gate generation disagrees with the feature gate registry");
  }
  return validate_cross_section(*this);
}

Status DurableStore::encode(const DurableState& state, std::string& out) {
  const Status valid = state.validate();
  if (valid.is_failure()) return valid;

  ByteWriter body;
  write_version(body, state.writer_version);
  body.generation(state.evolution_epoch);
  body.generation(state.coordinator_epoch);
  body.generation(state.matrix_generation);
  body.generation(state.gate_generation);
  body.generation(state.evidence_generation);
  body.generation(state.snapshot_generation);
  body.generation(state.policy_generation);
  body.generation(state.stage_generation);
  body.generation(state.migration_generation);
  body.generation(state.rollback_generation);
  body.generation(state.replay_watermark);

  const auto versions = state.components.all_versions();
  body.u32(static_cast<std::uint32_t>(versions.size()));
  for (const auto* version : versions) write_component_version(body, *version);

  const auto protocols = state.protocols.all();
  body.u32(static_cast<std::uint32_t>(protocols.size()));
  for (const auto* descriptor : protocols) write_protocol(body, *descriptor);

  const auto schemas = state.schemas.all();
  body.u32(static_cast<std::uint32_t>(schemas.size()));
  for (const auto* descriptor : schemas) write_schema(body, *descriptor);

  const auto edges = state.matrix.all_edges();
  body.u32(static_cast<std::uint32_t>(edges.size()));
  for (const auto* edge : edges) write_edge(body, *edge);

  const auto gates = state.gates.all();
  body.u32(static_cast<std::uint32_t>(gates.size()));
  for (const auto* gate : gates) write_gate(body, *gate);

  body.u32(static_cast<std::uint32_t>(state.plans.size()));
  for (const auto& [id, plan] : state.plans) {
    (void)id;
    write_plan(body, plan);
  }

  body.u32(static_cast<std::uint32_t>(state.migrations.size()));
  for (const auto& record : state.migrations) write_migration_record(body, record);

  body.u32(static_cast<std::uint32_t>(state.rollbacks.size()));
  for (const auto& record : state.rollbacks) write_rollback_record(body, record);

  body.u32(static_cast<std::uint32_t>(state.retirements.size()));
  for (const auto& [key, record] : state.retirements) {
    (void)key;
    write_retirement(body, record);
  }

  body.u32(static_cast<std::uint32_t>(state.fenced_boots.size()));
  for (const auto& [key, record] : state.fenced_boots) {
    (void)key;
    write_fenced_boot(body, record);
  }

  body.u32(static_cast<std::uint32_t>(state.workers.size()));
  for (const auto& [id, record] : state.workers) {
    (void)id;
    write_worker_record(body, record);
  }

  body.u32(static_cast<std::uint32_t>(state.stage_history.size()));
  for (const auto& record : state.stage_history) write_stage_record(body, record);

  const std::string_view body_view = body.view();
  if (body_view.size() > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "durable state exceeds the size bound");
  }

  ByteWriter out_writer;
  out_writer.raw(std::string_view(kEvolutionStateMagic, sizeof(kEvolutionStateMagic)));
  out_writer.u32(kEvolutionStateFormatVersion);
  out_writer.u64(static_cast<std::uint64_t>(body_view.size()));
  out_writer.u32(crc32(std::string(out_writer.view())));
  out_writer.digest(IntegrityDigest::of(body_view));
  out_writer.raw(body_view);
  out.assign(out_writer.view());
  return Status::ok();
}

Status DurableStore::decode(std::string_view bytes, DurableState& out) {
  if (bytes.size() < sizeof(kEvolutionStateMagic) + 4 + 8 + 4 + 16) {
    return Status::failure(ErrorCode::Truncated, "durable state is shorter than its header");
  }
  if (bytes.substr(0, sizeof(kEvolutionStateMagic)) !=
      std::string_view(kEvolutionStateMagic, sizeof(kEvolutionStateMagic))) {
    return Status::failure(ErrorCode::Corrupt, "durable state magic mismatch");
  }
  if (bytes.size() > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "durable state exceeds the size bound");
  }
  ByteReader reader(bytes);
  (void)reader.skip(sizeof(kEvolutionStateMagic));  // the magic was verified above
  const std::uint32_t version = reader.u32();
  const std::uint64_t body_size = reader.u64();
  const std::size_t crc_offset = reader.consumed();
  const std::uint32_t stored_crc = reader.u32();
  const IntegrityDigest stored_digest = reader.digest();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "durable state header is truncated");
  if (version != kEvolutionStateFormatVersion) {
    return Status::failure(ErrorCode::Unsupported, "durable state format version is not supported");
  }
  if (body_size > limits::kStateFileBytes || reader.remaining() != body_size) {
    return Status::failure(ErrorCode::Truncated, "durable state body length does not match the file");
  }
  if (crc32(bytes.substr(0, crc_offset)) != stored_crc) {
    return Status::failure(ErrorCode::IntegrityFailure, "durable state header checksum mismatch");
  }
  const std::string_view body = bytes.substr(crc_offset + 4 + 16);
  if (IntegrityDigest::of(body) != stored_digest) {
    return Status::failure(ErrorCode::IntegrityFailure, "durable state body digest mismatch");
  }

  // Decode into a temporary and only hand it to the caller once every section
  // has been validated: a failed load can never partially apply.
  DurableState temp;
  ByteReader body_reader(body);
  const char* what = "durable state body is malformed";
  Status status = read_version(body_reader, temp.writer_version, what);
  if (status.is_failure()) return status;
  temp.evolution_epoch = body_reader.generation<EvolutionEpochTag>();
  temp.coordinator_epoch = body_reader.generation<CoordinatorEpochTag>();
  temp.matrix_generation = body_reader.generation<CompatibilityGenerationTag>();
  temp.gate_generation = body_reader.generation<FeatureGateGenerationTag>();
  temp.evidence_generation = body_reader.generation<EvidenceGenerationTag>();
  temp.snapshot_generation = body_reader.generation<SnapshotGenerationTag>();
  temp.policy_generation = body_reader.generation<PolicyGenerationTag>();
  temp.stage_generation = body_reader.generation<StageGenerationTag>();
  temp.migration_generation = body_reader.generation<MigrationGenerationTag>();
  temp.rollback_generation = body_reader.generation<RollbackGenerationTag>();
  temp.replay_watermark = body_reader.generation<SnapshotGenerationTag>();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);

  const std::uint32_t component_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (component_count > limits::kComponentVersions) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < component_count; ++i) {
    RuntimeComponentVersion version_record;
    status = read_component_version(body_reader, version_record);
    if (status.is_failure()) return status;
    status = temp.components.restore(version_record);
    if (status.is_failure()) return status;
  }

  const std::uint32_t protocol_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (protocol_count > limits::kProtocolGenerations) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < protocol_count; ++i) {
    ProtocolDescriptor descriptor;
    status = read_protocol(body_reader, descriptor);
    if (status.is_failure()) return status;
    status = temp.protocols.publish(descriptor);
    if (status.is_failure()) return Status::failure(ErrorCode::Corrupt, "duplicate or invalid protocol record");
  }

  const std::uint32_t schema_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (schema_count > limits::kSchemaGenerations) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < schema_count; ++i) {
    SchemaDescriptor descriptor;
    status = read_schema(body_reader, descriptor);
    if (status.is_failure()) return status;
    status = temp.schemas.publish(descriptor);
    if (status.is_failure()) return Status::failure(ErrorCode::Corrupt, "duplicate or invalid schema record");
  }

  const std::uint32_t edge_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (edge_count > limits::kCompatibilityEdges) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < edge_count; ++i) {
    CompatibilityEdge edge;
    status = read_edge(body_reader, edge);
    if (status.is_failure()) return status;
    status = temp.matrix.upsert(edge);
    if (status.is_failure()) return status;
  }
  temp.matrix.set_generation(temp.matrix_generation);

  const std::uint32_t gate_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (gate_count > limits::kFeatureGates) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < gate_count; ++i) {
    FeatureGate gate;
    status = read_gate(body_reader, gate);
    if (status.is_failure()) return status;
    status = temp.gates.restore(gate);
    if (status.is_failure()) return status;
  }
  temp.gates.set_generation(temp.gate_generation);

  const std::uint32_t plan_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (plan_count > limits::kPlanHistory) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < plan_count; ++i) {
    EvolutionPlan plan;
    status = read_plan(body_reader, plan);    if (status.is_failure()) return status;
    if (plan.id.empty()) return Status::failure(ErrorCode::Corrupt, "durable plan has an empty id");
    if (!temp.plans.emplace(plan.id, plan).second) {
      return Status::failure(ErrorCode::Corrupt, "duplicate evolution plan id in durable state");
    }
  }

  const std::uint32_t migration_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (migration_count > limits::kMigrationRecords) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < migration_count; ++i) {
    MigrationRecord record;
    status = read_migration_record(body_reader, record);
    if (status.is_failure()) return status;
    temp.migrations.push_back(record);
  }

  const std::uint32_t rollback_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (rollback_count > limits::kRollbackRecords) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < rollback_count; ++i) {
    RollbackRecord record;
    status = read_rollback_record(body_reader, record);
    if (status.is_failure()) return status;
    temp.rollbacks.push_back(record);
  }

  const std::uint32_t retirement_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (retirement_count > limits::kRetiredGenerations) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < retirement_count; ++i) {
    RetirementRecord record;
    status = read_retirement(body_reader, record);
    if (status.is_failure()) return status;
    const auto key = std::make_pair(record.component, record.generation.raw());
    if (!temp.retirements.emplace(key, record).second) {
      return Status::failure(ErrorCode::Corrupt, "duplicate retirement record in durable state");
    }
  }

  const std::uint32_t fenced_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (fenced_count > limits::kFencedBoots) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < fenced_count; ++i) {
    FencedBoot record;
    status = read_fenced_boot(body_reader, record);
    if (status.is_failure()) return status;
    const auto key = std::make_pair(record.worker, record.boot.raw());
    if (!temp.fenced_boots.emplace(key, record).second) {
      return Status::failure(ErrorCode::Corrupt, "duplicate fenced boot record in durable state");
    }
  }

  const std::uint32_t worker_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (worker_count > limits::kWorkers) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < worker_count; ++i) {
    WorkerRecord record;
    status = read_worker_record(body_reader, record);
    if (status.is_failure()) return status;
    if (record.worker.empty()) {
      return Status::failure(ErrorCode::Corrupt, "durable worker record has an empty id");
    }
    if (!temp.workers.emplace(record.worker, record).second) {
      return Status::failure(ErrorCode::Corrupt, "duplicate worker record in durable state");
    }
  }

  const std::uint32_t stage_count = body_reader.u32();
  if (body_reader.failed()) return Status::failure(ErrorCode::Truncated, what);
  if (stage_count > limits::kStageHistory) return Status::failure(ErrorCode::Corrupt, what);
  for (std::uint32_t i = 0; i < stage_count; ++i) {
    PlanStageRecord record;
    status = read_stage_record(body_reader, record);
    if (status.is_failure()) return status;
    temp.stage_history.push_back(record);
  }

  if (!body_reader.at_end()) {
    return Status::failure(ErrorCode::Corrupt, "durable state body has trailing bytes");
  }

  const Status validated = temp.validate();
  if (validated.is_failure()) return validated;

  out = std::move(temp);
  return Status::ok();
}

Status DurableStore::save(const DurableState& state) const {
  std::string bytes;
  const Status encoded = encode(state, bytes);
  if (encoded.is_failure()) return encoded;
  return write_file_atomic(path_, bytes);
}

Status DurableStore::load(DurableState& out) const {
  std::string bytes;
  const Status read = read_file_bounded(path_, limits::kStateFileBytes, bytes);
  if (read.is_failure()) return read;
  return decode(bytes, out);
}

}  // namespace ref
