// Runtime Evolution Fabric - state schema generations.
//
// Read compatibility and write compatibility are separate declarations on
// purpose: a runtime that can read old state is not therefore allowed to write
// state the old runtime can no longer read.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/compat.hpp"
#include "ref/ids.hpp"
#include "ref/support.hpp"

namespace ref {

enum class FieldType : std::uint8_t { U64 = 0, I64, Boolean, Text, Bytes, Digest };
[[nodiscard]] const char* to_string(FieldType type) noexcept;
[[nodiscard]] std::optional<FieldType> parse_field_type(std::string_view text) noexcept;

enum class CanonicalizationRule : std::uint8_t {
  None = 0,
  FixedFieldOrder,
  SortedFieldNames,
  FixedFieldOrderStableEncoding,
};
[[nodiscard]] const char* to_string(CanonicalizationRule rule) noexcept;

enum class IntegritySemantics : std::uint8_t {
  None = 0,
  Checksum,
  ChecksumAndSchemaBinding,
};
[[nodiscard]] const char* to_string(IntegritySemantics semantics) noexcept;

struct SchemaField {
  FieldId id{};
  FieldType type{FieldType::U64};
  SchemaGeneration introduced_in{};
  SchemaGeneration removed_in{};  // unset while present
  bool required{false};
  // True when the field cannot be reconstructed by a reverse migration: once a
  // schema generation carrying an irreversible field is committed, rolling back
  // to an older generation loses information permanently.
  bool irreversible{false};
  NoteText required_default{};
};

struct SchemaDescriptor {
  SchemaId id{};
  SchemaGeneration generation{};
  std::vector<SchemaField> fields{};
  // Stored formats this generation can interpret.
  SchemaGenerationRange readable_formats{};
  // Formats this generation may emit (its own, plus any legacy write modes).
  SchemaGenerationRange writable_formats{};
  // Target of a reverse migration, when one exists.
  SchemaGeneration reverse_migration_to{};
  bool requires_explicit_commit{true};
  CanonicalizationRule canonicalization{CanonicalizationRule::FixedFieldOrderStableEncoding};
  IntegritySemantics integrity{IntegritySemantics::ChecksumAndSchemaBinding};
  EvidenceClass provenance{EvidenceClass::Unknown};
  EvidenceGeneration evidence{};
  DetailText notes{};

  [[nodiscard]] Status validate() const noexcept;
  [[nodiscard]] bool can_read_format(SchemaGeneration format) const noexcept {
    return readable_formats.contains(format);
  }
  [[nodiscard]] bool can_write_format(SchemaGeneration format) const noexcept {
    return writable_formats.contains(format);
  }
  [[nodiscard]] const SchemaField* find_field(const FieldId& field) const noexcept;
  [[nodiscard]] bool has_irreversible_field() const noexcept;
};

// Structured read/write compatibility answers.
struct SchemaCompatResult {
  CompatOutcome outcome{CompatOutcome::Unknown};
  DetailText detail{};
  [[nodiscard]] bool is_blocking() const noexcept { return is_blocking_outcome(outcome); }
};

class SchemaRegistry {
 public:
  SchemaRegistry() = default;

  Status publish(const SchemaDescriptor& descriptor);
  [[nodiscard]] const SchemaDescriptor* find(const SchemaId& id,
                                             SchemaGeneration generation) const noexcept;
  [[nodiscard]] SchemaGeneration highest(const SchemaId& id) const noexcept;
  [[nodiscard]] std::vector<const SchemaDescriptor*> all() const;
  [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }
  [[nodiscard]] Status validate() const;

  // Can a runtime at 'reader' generation interpret state stored at 'stored'?
  [[nodiscard]] SchemaCompatResult read_compatibility(const SchemaId& id, SchemaGeneration reader,
                                                      SchemaGeneration stored) const;
  // Can a runtime at 'writer' generation emit a form that a consumer at
  // 'consumer' generation can interpret?
  [[nodiscard]] SchemaCompatResult write_compatibility(const SchemaId& id, SchemaGeneration writer,
                                                       SchemaGeneration consumer) const;
  // Whether a reverse migration path exists out of 'generation'.
  [[nodiscard]] bool has_reverse_migration(const SchemaId& id, SchemaGeneration generation) const noexcept;

 private:
  std::map<std::pair<SchemaId, std::uint64_t>, SchemaDescriptor> by_id_{};
};

// Publishes the built-in component-state schema generations (1..3) into a
// registry. Shared by the coordinator and by runtime workers so both sides
// agree on read/write compatibility without duplicating the definition.
[[nodiscard]] Status install_builtin_state_schemas(SchemaRegistry& registry);

}  // namespace ref
