// Runtime Evolution Fabric - governed state migration.
//
// Migration is the point where a live runtime can destroy its own ability to
// roll back, so it is modelled as a governed, transactional operation:
//
//   inspect -> verify source generation -> prepare -> stage temporary state ->
//   validate -> commit generation transition -> preserve rollback metadata
//
// The authoritative state file is never mutated in place: a staged file is
// written, verified, and then atomically renamed over the target.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ref/ids.hpp"
#include "ref/schema.hpp"
#include "ref/support.hpp"

namespace ref {

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------
enum class MigrationOutcome : std::uint8_t {
  NotRequired = 0,
  Ready,
  Committed,
  Blocked,
  Failed,
  RollbackAvailable,
  Irreversible,
  RevalidationRequired,
  OutcomeUnknown,
};

inline constexpr std::size_t kMigrationOutcomeCount = 9;
[[nodiscard]] const char* to_string(MigrationOutcome outcome) noexcept;
[[nodiscard]] std::optional<MigrationOutcome> parse_migration_outcome(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Durable state object
// ---------------------------------------------------------------------------
struct StateValue {
  FieldId name{};
  FieldType type{FieldType::U64};
  std::uint64_t number{0};      // U64, I64 (two's complement) and BOOL
  std::string text{};           // TEXT and BYTES
  IntegrityDigest digest{};     // DIGEST
};

// Deterministic, canonical, ordered map of schema fields: the unit that schema
// generations describe and migrations transform.
class StateObject {
 public:
  StateObject() = default;

  Status set_u64(const FieldId& name, std::uint64_t value);
  Status set_i64(const FieldId& name, std::int64_t value);
  Status set_bool(const FieldId& name, bool value);
  Status set_text(const FieldId& name, std::string_view value);
  Status set_digest(const FieldId& name, IntegrityDigest value);
  Status remove_field(const FieldId& name);

  [[nodiscard]] const StateValue* find(const FieldId& name) const noexcept;
  [[nodiscard]] std::vector<StateValue> values() const;
  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

  // Canonical encoding: fields sorted by name, fixed value encoding.
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static Status decode(std::string_view bytes, StateObject& out);
  // Required fields present, types matching, no unexpected fields.
  [[nodiscard]] Status validate_against(const SchemaDescriptor& descriptor) const;

 private:
  Status assign(StateValue value);
  std::vector<StateValue> values_{};  // kept sorted by field name
};

// ---------------------------------------------------------------------------
// Durable state file
// ---------------------------------------------------------------------------
inline constexpr char kStateFileMagic[8] = {'R', 'E', 'F', 'S', 'T', 'A', 'T', '1'};
inline constexpr StateFormatGeneration kStateFormatGenerationV1 = StateFormatGeneration::from_raw(1);

struct StateFileHeader {
  StateFormatGeneration format{};
  SchemaId schema{};
  SchemaGeneration generation{};
  MigrationGeneration migration{};
  EvolutionEpoch epoch{};
  PolicyGeneration policy{};
  IntegrityDigest payload_digest{};
  std::uint64_t payload_size{0};
  std::uint32_t crc{0};

  [[nodiscard]] bool is_set() const noexcept {
    return format.is_set() && generation.is_set() && !schema.empty();
  }
};

struct StateFile {
  StateFileHeader header{};
  std::string payload{};
};

[[nodiscard]] Status encode_state_file(const StateFile& file, std::string& out);
[[nodiscard]] Status decode_state_file(std::string_view bytes, StateFile& out);
[[nodiscard]] Status write_state_file(const std::string& path, const StateFile& file);
[[nodiscard]] Status read_state_file(const std::string& path, StateFile& out);

// ---------------------------------------------------------------------------
// Migration contract and results
// ---------------------------------------------------------------------------
struct MigrationContract {
  MigrationId id{};
  MigrationGeneration generation{};
  SchemaId schema{};
  SchemaGeneration source{};
  SchemaGeneration target{};
  RuntimeGeneration runtime_generation{};
  EvolutionEpoch epoch{};
  PolicyGeneration policy{};
  EvolutionPlanId plan{};
  bool irreversible{false};
  bool preserve_rollback_metadata{true};
  bool irreversible_acknowledged{false};

  [[nodiscard]] Status validate() const noexcept;
};

struct MigrationResult {
  Status status{};
  MigrationOutcome outcome{MigrationOutcome::OutcomeUnknown};
  SchemaGeneration observed_generation{};
  IntegrityDigest source_digest{};
  IntegrityDigest target_digest{};
  bool rollback_metadata_present{false};
  bool rollback_available{false};
  bool irreversible{false};
  DetailText detail{};

  [[nodiscard]] bool is_ok() const noexcept { return status.is_ok(); }
};

struct MigrationRecord {
  MigrationId id{};
  MigrationGeneration generation{};
  SchemaId schema{};
  SchemaGeneration source{};
  SchemaGeneration target{};
  RuntimeGeneration runtime_generation{};
  EvolutionEpoch epoch{};
  PolicyGeneration policy{};
  EvolutionPlanId plan{};
  IntegrityDigest source_digest{};
  IntegrityDigest target_digest{};
  bool irreversible{false};
  bool rollback_metadata_preserved{false};
  bool rollback_available{false};
  MigrationOutcome outcome{MigrationOutcome::OutcomeUnknown};
  EvidenceClass provenance{EvidenceClass::Unknown};
  StageGeneration recorded_at{};
  DetailText detail{};
};

class StateMigrator {
 public:
  explicit StateMigrator(const SchemaRegistry* schemas) noexcept : schemas_(schemas) {}

  // Dry run: validates everything except the commit itself.
  [[nodiscard]] MigrationResult plan_migration(const std::string& path,
                                               const MigrationContract& contract) const;
  [[nodiscard]] MigrationResult inspect(const std::string& path) const;
  [[nodiscard]] MigrationResult migrate(const std::string& path, const MigrationContract& contract) const;
  [[nodiscard]] MigrationResult reverse(const std::string& path, const MigrationContract& contract) const;
  // Reconciliation after an ambiguous outcome (for example the worker died
  // between commit and acknowledgement).
  [[nodiscard]] MigrationResult reconcile(const std::string& path,
                                          const MigrationContract& contract) const;

  [[nodiscard]] static bool transform_supported(const SchemaId& schema, SchemaGeneration from,
                                                SchemaGeneration to) noexcept;
  [[nodiscard]] static bool transform_is_irreversible(const SchemaId& schema, SchemaGeneration from,
                                                      SchemaGeneration to) noexcept;
  [[nodiscard]] static std::string checkpoint_path(const std::string& path);
  [[nodiscard]] static std::string staging_path(const std::string& path);

 private:
  const SchemaRegistry* schemas_;
};

}  // namespace ref
