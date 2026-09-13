#include "ref/schema.hpp"

#include <algorithm>

namespace ref {

const char* to_string(FieldType type) noexcept {
  switch (type) {
    case FieldType::U64: return "U64";
    case FieldType::I64: return "I64";
    case FieldType::Boolean: return "BOOL";
    case FieldType::Text: return "TEXT";
    case FieldType::Bytes: return "BYTES";
    case FieldType::Digest: return "DIGEST";
  }
  return "U64";
}

std::optional<FieldType> parse_field_type(std::string_view text) noexcept {
  if (text == "U64") return FieldType::U64;
  if (text == "I64") return FieldType::I64;
  if (text == "BOOL") return FieldType::Boolean;
  if (text == "TEXT") return FieldType::Text;
  if (text == "BYTES") return FieldType::Bytes;
  if (text == "DIGEST") return FieldType::Digest;
  return std::nullopt;
}

const char* to_string(CanonicalizationRule rule) noexcept {
  switch (rule) {
    case CanonicalizationRule::None: return "NONE";
    case CanonicalizationRule::FixedFieldOrder: return "FIXED_FIELD_ORDER";
    case CanonicalizationRule::SortedFieldNames: return "SORTED_FIELD_NAMES";
    case CanonicalizationRule::FixedFieldOrderStableEncoding: return "FIXED_FIELD_ORDER_STABLE_ENCODING";
  }
  return "NONE";
}

const char* to_string(IntegritySemantics semantics) noexcept {
  switch (semantics) {
    case IntegritySemantics::None: return "NONE";
    case IntegritySemantics::Checksum: return "CHECKSUM";
    case IntegritySemantics::ChecksumAndSchemaBinding: return "CHECKSUM_AND_SCHEMA_BINDING";
  }
  return "NONE";
}

Status SchemaDescriptor::validate() const noexcept {
  if (id.empty()) return Status::failure(ErrorCode::InvalidArgument, "schema id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "schema generation is unset");
  if (fields.empty()) return Status::failure(ErrorCode::InvalidArgument, "schema declares no fields");
  if (fields.size() > limits::kMessageFields) {
    return Status::failure(ErrorCode::LimitExceeded, "schema declares too many fields");
  }
  if (!readable_formats.is_valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "schema readable format range is invalid");
  }
  if (!writable_formats.is_valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "schema writable format range is invalid");
  }
  if (!readable_formats.contains(generation)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "schema generation cannot read its own stored format");
  }
  if (!writable_formats.contains(generation)) {
    return Status::failure(ErrorCode::InvalidArgument, "schema generation cannot write its own format");
  }
  if (canonicalization == CanonicalizationRule::None) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "schema generation must declare a canonicalization rule");
  }
  if (reverse_migration_to.is_set() && reverse_migration_to >= generation) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "reverse migration target must be older than the schema generation");
  }
  if (has_irreversible_field() && reverse_migration_to.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "schema generation declares an irreversible field and a reverse migration");
  }
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto& field = fields[i];
    if (field.id.empty()) return Status::failure(ErrorCode::InvalidArgument, "schema field id is empty");
    if (!field.introduced_in.is_set() || field.introduced_in > generation) {
      return Status::failure(ErrorCode::InvalidArgument, "schema field introduced beyond the generation");
    }
    if (field.removed_in.is_set()) {
      if (field.removed_in <= field.introduced_in) {
        return Status::failure(ErrorCode::InvalidArgument, "schema field removed before it was introduced");
      }
      if (field.required) {
        return Status::failure(ErrorCode::InvalidArgument, "removed schema field cannot be required");
      }
    }
    for (std::size_t j = i + 1; j < fields.size(); ++j) {
      if (fields[j].id == field.id) {
        return Status::failure(ErrorCode::InvalidArgument, "duplicate schema field id");
      }
    }
  }
  if (provenance == EvidenceClass::Unknown) {
    return Status::failure(ErrorCode::InvalidArgument, "schema descriptor has no evidence provenance");
  }
  return Status::ok();
}

const SchemaField* SchemaDescriptor::find_field(const FieldId& field) const noexcept {
  for (const auto& entry : fields) {
    if (entry.id == field) return &entry;
  }
  return nullptr;
}

bool SchemaDescriptor::has_irreversible_field() const noexcept {
  for (const auto& field : fields) {
    if (field.irreversible && field.introduced_in <= generation &&
        (!field.removed_in.is_set() || field.removed_in > generation)) {
      return true;
    }
  }
  return false;
}

Status SchemaRegistry::publish(const SchemaDescriptor& descriptor) {
  const Status valid = descriptor.validate();
  if (valid.is_failure()) return valid;
  const auto key = std::make_pair(descriptor.id, descriptor.generation.raw());
  const auto existing = by_id_.find(key);
  if (existing != by_id_.end()) {
    // Republishing is only legal when the structural contract is unchanged.
    if (existing->second.readable_formats != descriptor.readable_formats ||
        existing->second.writable_formats != descriptor.writable_formats ||
        existing->second.canonicalization != descriptor.canonicalization) {
      return Status::failure(ErrorCode::Conflict,
                             "schema generation republished with different format compatibility");
    }
    existing->second = descriptor;
    return Status::ok();
  }
  if (by_id_.size() >= limits::kSchemaGenerations) {
    return Status::failure(ErrorCode::LimitExceeded, "schema generation bound reached");
  }
  by_id_.emplace(key, descriptor);
  return Status::ok();
}

const SchemaDescriptor* SchemaRegistry::find(const SchemaId& id,
                                             SchemaGeneration generation) const noexcept {
  const auto it = by_id_.find(std::make_pair(id, generation.raw()));
  return it == by_id_.end() ? nullptr : &it->second;
}

SchemaGeneration SchemaRegistry::highest(const SchemaId& id) const noexcept {
  SchemaGeneration best{};
  for (const auto& [key, descriptor] : by_id_) {
    if (key.first != id) continue;
    if (!best.is_set() || descriptor.generation > best) best = descriptor.generation;
  }
  return best;
}

std::vector<const SchemaDescriptor*> SchemaRegistry::all() const {
  std::vector<const SchemaDescriptor*> out;
  out.reserve(by_id_.size());
  for (const auto& [key, descriptor] : by_id_) {
    (void)key;
    out.push_back(&descriptor);
  }
  return out;
}

Status SchemaRegistry::validate() const {
  if (by_id_.size() > limits::kSchemaGenerations) {
    return Status::failure(ErrorCode::Corrupt, "schema registry exceeds the generation bound");
  }
  for (const auto& [key, descriptor] : by_id_) {
    if (key.first != descriptor.id || key.second != descriptor.generation.raw()) {
      return Status::failure(ErrorCode::Corrupt, "schema registry key does not match its descriptor");
    }
    const Status valid = descriptor.validate();
    if (valid.is_failure()) return valid;
  }
  return Status::ok();
}

SchemaCompatResult SchemaRegistry::read_compatibility(const SchemaId& id, SchemaGeneration reader,
                                                      SchemaGeneration stored) const {
  SchemaCompatResult result;
  const SchemaDescriptor* reader_descriptor = find(id, reader);
  if (reader_descriptor == nullptr) {
    result.outcome = CompatOutcome::Unknown;
    result.detail = DetailText::from_valid("reader schema generation is not registered");
    return result;
  }
  const SchemaDescriptor* stored_descriptor = find(id, stored);
  if (stored_descriptor == nullptr) {
    result.outcome = CompatOutcome::Unknown;
    result.detail = DetailText::from_valid("stored schema generation is not registered");
    return result;
  }
  if (reader_descriptor->can_read_format(stored)) {
    result.outcome = reader == stored ? CompatOutcome::FullyCompatible : CompatOutcome::ReadCompatible;
    result.detail = DetailText::from_valid(reader == stored ? "same schema generation"
                                                            : "older stored format is readable");
    return result;
  }
  result.outcome = CompatOutcome::IncompatibleSchema;
  result.detail = DetailText::from_valid("reader does not declare the stored format as readable");
  return result;
}

SchemaCompatResult SchemaRegistry::write_compatibility(const SchemaId& id, SchemaGeneration writer,
                                                       SchemaGeneration consumer) const {
  SchemaCompatResult result;
  const SchemaDescriptor* writer_descriptor = find(id, writer);
  if (writer_descriptor == nullptr) {
    result.outcome = CompatOutcome::Unknown;
    result.detail = DetailText::from_valid("writer schema generation is not registered");
    return result;
  }
  const SchemaDescriptor* consumer_descriptor = find(id, consumer);
  if (consumer_descriptor == nullptr) {
    result.outcome = CompatOutcome::Unknown;
    result.detail = DetailText::from_valid("consumer schema generation is not registered");
    return result;
  }
  // Find any format the writer may emit that the consumer declares readable.
  for (SchemaGeneration format = writer_descriptor->writable_formats.min;
       format.is_set() && format <= writer_descriptor->writable_formats.max; format = format.next()) {
    if (consumer_descriptor->can_read_format(format)) {
      if (writer == consumer) {
        result.outcome = CompatOutcome::FullyCompatible;
        result.detail = DetailText::from_valid("same schema generation");
      } else if (format == consumer) {
        result.outcome = CompatOutcome::WriteCompatible;
        result.detail = DetailText::from_valid("writer can emit the consumer's own format");
      } else {
        result.outcome = CompatOutcome::MixedVersionCompatible;
        result.detail = DetailText::from_valid("writer and consumer share a common stored format");
      }
      return result;
    }
  }
  result.outcome = CompatOutcome::IncompatibleSchema;
  result.detail = DetailText::from_valid("no format exists that the writer may emit and the consumer reads");
  return result;
}

bool SchemaRegistry::has_reverse_migration(const SchemaId& id, SchemaGeneration generation) const noexcept {
  const SchemaDescriptor* descriptor = find(id, generation);
  if (descriptor == nullptr) return false;
  return descriptor->reverse_migration_to.is_set();
}

// ---------------------------------------------------------------------------
// Built-in component state schema generations
// ---------------------------------------------------------------------------
Status install_builtin_state_schemas(SchemaRegistry& registry) {
  const SchemaId id = SchemaId::from_valid("component-state");

  SchemaDescriptor v1;
  v1.id = id;
  v1.generation = SchemaGeneration::from_raw(1);
  v1.readable_formats = SchemaGenerationRange::single(SchemaGeneration::from_raw(1));
  v1.writable_formats = SchemaGenerationRange::single(SchemaGeneration::from_raw(1));
  v1.provenance = EvidenceClass::Real;
  v1.evidence = EvidenceGeneration::first();
  v1.notes = NoteText::from_valid("baseline component state");
  {
    SchemaField owner;
    owner.id = FieldId::from_valid("owner");
    owner.type = FieldType::Text;
    owner.introduced_in = SchemaGeneration::from_raw(1);
    owner.required = true;
    v1.fields.push_back(owner);

    SchemaField token;
    token.id = FieldId::from_valid("legacy_token");
    token.type = FieldType::Text;
    token.introduced_in = SchemaGeneration::from_raw(1);
    token.required = false;
    v1.fields.push_back(token);

    SchemaField counter;
    counter.id = FieldId::from_valid("mutation_counter");
    counter.type = FieldType::U64;
    counter.introduced_in = SchemaGeneration::from_raw(1);
    counter.required = true;
    v1.fields.push_back(counter);
  }
  Status status = registry.publish(v1);
  if (status.is_failure()) return status;

  SchemaDescriptor v2 = v1;
  v2.generation = SchemaGeneration::from_raw(2);
  v2.readable_formats = SchemaGenerationRange::closed(SchemaGeneration::from_raw(1), SchemaGeneration::from_raw(2));
  v2.writable_formats = SchemaGenerationRange::closed(SchemaGeneration::from_raw(1), SchemaGeneration::from_raw(2));
  v2.reverse_migration_to = SchemaGeneration::from_raw(1);
  v2.notes = NoteText::from_valid("adds the required state epoch; can still emit the v1 form");
  {
    SchemaField epoch;
    epoch.id = FieldId::from_valid("state_epoch");
    epoch.type = FieldType::U64;
    epoch.introduced_in = SchemaGeneration::from_raw(2);
    epoch.required = true;
    epoch.required_default = NoteText::from_valid("1");
    v2.fields.push_back(epoch);
  }
  status = registry.publish(v2);
  if (status.is_failure()) return status;

  SchemaDescriptor v3 = v2;
  v3.generation = SchemaGeneration::from_raw(3);
  v3.readable_formats = SchemaGenerationRange::closed(SchemaGeneration::from_raw(1), SchemaGeneration::from_raw(3));
  v3.writable_formats = SchemaGenerationRange::single(SchemaGeneration::from_raw(3));
  v3.reverse_migration_to = SchemaGeneration::unset();
  v3.notes = NoteText::from_valid("replaces the legacy token with an irreversible digest");
  for (auto& field : v3.fields) {
    if (field.id == FieldId::from_valid("legacy_token")) {
      field.removed_in = SchemaGeneration::from_raw(3);
      field.required = false;
    }
  }
  SchemaField token_digest;
  token_digest.id = FieldId::from_valid("token_digest");
  token_digest.type = FieldType::Digest;
  token_digest.introduced_in = SchemaGeneration::from_raw(3);
  token_digest.required = true;
  token_digest.irreversible = true;
  v3.fields.push_back(token_digest);
  return registry.publish(v3);
}

}  // namespace ref
