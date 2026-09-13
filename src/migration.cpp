#include "ref/migration.hpp"

#include <algorithm>
#include <array>

namespace ref {
namespace {

constexpr std::array<const char*, kMigrationOutcomeCount> kOutcomeNames{
    "MIGRATION_NOT_REQUIRED",   "MIGRATION_READY",  "MIGRATION_COMMITTED",
    "MIGRATION_BLOCKED",        "MIGRATION_FAILED", "MIGRATION_ROLLBACK_AVAILABLE",
    "MIGRATION_IRREVERSIBLE",   "REVALIDATION_REQUIRED", "OUTCOME_UNKNOWN"};

const SchemaId& component_state_schema() {
  static const SchemaId id = SchemaId::from_valid("component-state");
  return id;
}

bool same_std_string(const std::string& left, const char* right) { return left == right; }

}  // namespace

const char* to_string(MigrationOutcome outcome) noexcept {
  const auto index = static_cast<std::size_t>(outcome);
  return index < kOutcomeNames.size() ? kOutcomeNames[index] : "OUTCOME_UNKNOWN";
}

std::optional<MigrationOutcome> parse_migration_outcome(std::string_view text) noexcept {
  for (std::size_t i = 0; i < kOutcomeNames.size(); ++i) {
    if (text == kOutcomeNames[i]) return static_cast<MigrationOutcome>(i);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// StateObject
// ---------------------------------------------------------------------------
Status StateObject::assign(StateValue value) {
  if (value.name.empty()) return Status::failure(ErrorCode::InvalidArgument, "state field name is empty");
  const auto position =
      std::lower_bound(values_.begin(), values_.end(), value.name,
                       [](const StateValue& entry, const FieldId& name) { return entry.name < name; });
  if (position != values_.end() && position->name == value.name) {
    *position = std::move(value);
    return Status::ok();
  }
  if (values_.size() >= limits::kMessageFields) {
    return Status::failure(ErrorCode::LimitExceeded, "state object field bound reached");
  }
  values_.insert(position, std::move(value));
  return Status::ok();
}

Status StateObject::set_u64(const FieldId& name, std::uint64_t value) {
  StateValue entry;
  entry.name = name;
  entry.type = FieldType::U64;
  entry.number = value;
  return assign(std::move(entry));
}

Status StateObject::set_i64(const FieldId& name, std::int64_t value) {
  StateValue entry;
  entry.name = name;
  entry.type = FieldType::I64;
  entry.number = static_cast<std::uint64_t>(value);
  return assign(std::move(entry));
}

Status StateObject::set_bool(const FieldId& name, bool value) {
  StateValue entry;
  entry.name = name;
  entry.type = FieldType::Boolean;
  entry.number = value ? 1u : 0u;
  return assign(std::move(entry));
}

Status StateObject::set_text(const FieldId& name, std::string_view value) {
  if (value.size() > limits::kTextBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "state text field exceeds the text bound");
  }
  for (const char c : value) {
    if (!is_valid_text_char(c)) {
      return Status::failure(ErrorCode::InvalidArgument, "state text field contains control characters");
    }
  }
  StateValue entry;
  entry.name = name;
  entry.type = FieldType::Text;
  entry.text.assign(value);
  return assign(std::move(entry));
}

Status StateObject::set_digest(const FieldId& name, IntegrityDigest value) {
  StateValue entry;
  entry.name = name;
  entry.type = FieldType::Digest;
  entry.digest = value;
  return assign(std::move(entry));
}

Status StateObject::remove_field(const FieldId& name) {
  const auto position =
      std::lower_bound(values_.begin(), values_.end(), name,
                       [](const StateValue& entry, const FieldId& key) { return entry.name < key; });
  if (position == values_.end() || position->name != name) {
    return Status::failure(ErrorCode::NotFound, "state field is not present");
  }
  values_.erase(position);
  return Status::ok();
}

const StateValue* StateObject::find(const FieldId& name) const noexcept {
  const auto position =
      std::lower_bound(values_.begin(), values_.end(), name,
                       [](const StateValue& entry, const FieldId& key) { return entry.name < key; });
  if (position == values_.end() || position->name != name) return nullptr;
  return &*position;
}

std::vector<StateValue> StateObject::values() const { return values_; }

std::string StateObject::encode() const {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(values_.size()));
  for (const auto& entry : values_) {
    writer.ident(entry.name);
    writer.u8(static_cast<std::uint8_t>(entry.type));
    switch (entry.type) {
      case FieldType::U64:
      case FieldType::I64:
      case FieldType::Boolean:
        writer.u64(entry.number);
        break;
      case FieldType::Text:
      case FieldType::Bytes:
        writer.text(entry.text, limits::kTextBytes);
        break;
      case FieldType::Digest:
        writer.digest(entry.digest);
        break;
    }
  }
  return std::string(writer.view());
}

Status StateObject::decode(std::string_view bytes, StateObject& out) {
  out = StateObject{};
  ByteReader reader(bytes);
  const std::uint32_t count = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "state payload header is truncated");
  if (count > limits::kMessageFields) {
    return Status::failure(ErrorCode::LimitExceeded, "state payload declares too many fields");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const FieldId name = reader.ident<FieldIdTag, 63>();
    const std::uint8_t type_raw = reader.u8();
    if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "state payload field is malformed");
    if (type_raw > static_cast<std::uint8_t>(FieldType::Digest)) {
      return Status::failure(ErrorCode::Corrupt, "state payload field type is unknown");
    }
    const auto type = static_cast<FieldType>(type_raw);
    Status status;
    switch (type) {
      case FieldType::U64: {
        const std::uint64_t value = reader.u64();
        status = out.set_u64(name, value);
        break;
      }
      case FieldType::I64: {
        const std::uint64_t raw = reader.u64();
        status = out.set_i64(name, static_cast<std::int64_t>(raw));
        break;
      }
      case FieldType::Boolean: {
        const std::uint64_t raw = reader.u64();
        if (raw > 1) return Status::failure(ErrorCode::Corrupt, "state payload boolean is not 0 or 1");
        status = out.set_bool(name, raw == 1);
        break;
      }
      case FieldType::Text:
      case FieldType::Bytes: {
        const std::string_view text = reader.bytes(limits::kTextBytes);
        if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "state payload text is truncated");
        status = out.set_text(name, text);
        break;
      }
      case FieldType::Digest: {
        const IntegrityDigest digest = reader.digest();
        status = out.set_digest(name, digest);
        break;
      }
    }
    if (status.is_failure()) return status;
    if (reader.failed()) return Status::failure(ErrorCode::Corrupt, "state payload field is malformed");
  }
  if (!reader.at_end()) return Status::failure(ErrorCode::Corrupt, "state payload has trailing bytes");
  return Status::ok();
}

Status StateObject::validate_against(const SchemaDescriptor& descriptor) const {
  for (const auto& field : descriptor.fields) {
    const bool present = !field.removed_in.is_set() || field.removed_in > descriptor.generation;
    const StateValue* value = find(field.id);
    if (!present) {
      if (value != nullptr) {
        return Status::failure(ErrorCode::Corrupt, "state object carries a removed schema field");
      }
      continue;
    }
    const bool introduced = field.introduced_in <= descriptor.generation;
    if (!introduced) {
      if (value != nullptr) {
        return Status::failure(ErrorCode::Corrupt, "state object carries a field not yet introduced");
      }
      continue;
    }
    if (value == nullptr) {
      if (field.required) {
        return Status::failure(ErrorCode::Corrupt, "state object is missing a required schema field");
      }
      continue;
    }
    if (value->type != field.type) {
      return Status::failure(ErrorCode::Corrupt, "state object field type does not match the schema");
    }
  }
  for (const auto& entry : values_) {
    const SchemaField* field = descriptor.find_field(entry.name);
    if (field == nullptr) {
      return Status::failure(ErrorCode::Corrupt, "state object carries an undeclared field");
    }
    if (entry.type != field->type) {
      return Status::failure(ErrorCode::Corrupt, "state object field type does not match the schema");
    }
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// State file container
// ---------------------------------------------------------------------------
Status encode_state_file(const StateFile& file, std::string& out) {
  if (!file.header.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "state file header is incomplete");
  }
  if (file.payload.size() > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "state payload exceeds the size bound");
  }
  ByteWriter writer;
  writer.raw(std::string_view(kStateFileMagic, sizeof(kStateFileMagic)));
  writer.generation(file.header.format);
  writer.ident(file.header.schema);
  writer.generation(file.header.generation);
  writer.generation(file.header.migration);
  writer.generation(file.header.epoch);
  writer.generation(file.header.policy);
  writer.u64(static_cast<std::uint64_t>(file.payload.size()));
  writer.digest(IntegrityDigest::of(file.payload));
  const std::uint32_t crc = crc32(std::string(writer.view()));
  writer.u32(crc);
  const std::string header_bytes(writer.view());
  writer.raw(file.payload);
  out.assign(writer.view());
  (void)header_bytes;
  return Status::ok();
}

Status decode_state_file(std::string_view bytes, StateFile& out) {
  out = StateFile{};
  if (bytes.size() < sizeof(kStateFileMagic)) {
    return Status::failure(ErrorCode::Truncated, "state file is shorter than its magic");
  }
  if (bytes.substr(0, sizeof(kStateFileMagic)) != std::string_view(kStateFileMagic, sizeof(kStateFileMagic))) {
    return Status::failure(ErrorCode::Corrupt, "state file magic mismatch");
  }
  if (bytes.size() > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "state file exceeds the size bound");
  }
  ByteReader reader(bytes);
  (void)reader.skip(sizeof(kStateFileMagic));  // the magic was verified above
  StateFileHeader header;
  header.format = reader.generation<StateFormatGenerationTag>();
  header.schema = reader.ident<SchemaIdTag, 63>();
  header.generation = reader.generation<SchemaGenerationTag>();
  header.migration = reader.generation<MigrationGenerationTag>();
  header.epoch = reader.generation<EvolutionEpochTag>();
  header.policy = reader.generation<PolicyGenerationTag>();
  header.payload_size = reader.u64();
  header.payload_digest = reader.digest();
  const std::size_t crc_offset = reader.consumed();
  const std::uint32_t stored_crc = reader.u32();
  if (reader.failed()) return Status::failure(ErrorCode::Truncated, "state file header is truncated");
  if (header.format != kStateFormatGenerationV1) {
    return Status::failure(ErrorCode::Unsupported, "state format generation is not supported");
  }
  if (header.payload_size > limits::kStateFileBytes) {
    return Status::failure(ErrorCode::LimitExceeded, "state payload length exceeds the size bound");
  }
  if (reader.remaining() != header.payload_size) {
    return Status::failure(ErrorCode::Truncated, "state payload length does not match the file body");
  }
  const std::uint32_t computed = crc32(bytes.substr(0, crc_offset));
  if (computed != stored_crc) {
    return Status::failure(ErrorCode::IntegrityFailure, "state file header checksum mismatch");
  }
  out.header = header;
  out.payload.assign(bytes.substr(crc_offset + 4));
  if (IntegrityDigest::of(out.payload) != header.payload_digest) {
    out = StateFile{};
    return Status::failure(ErrorCode::IntegrityFailure, "state payload digest mismatch");
  }
  return Status::ok();
}

Status write_state_file(const std::string& path, const StateFile& file) {
  std::string bytes;
  const Status encoded = encode_state_file(file, bytes);
  if (encoded.is_failure()) return encoded;
  return write_file_atomic(path, bytes);
}

Status read_state_file(const std::string& path, StateFile& out) {
  std::string bytes;
  const Status read = read_file_bounded(path, limits::kStateFileBytes, bytes);
  if (read.is_failure()) return read;
  return decode_state_file(bytes, out);
}

// ---------------------------------------------------------------------------
// Migration contract
// ---------------------------------------------------------------------------
Status MigrationContract::validate() const noexcept {
  if (id.empty()) return Status::failure(ErrorCode::InvalidArgument, "migration id is empty");
  if (!generation.is_set()) return Status::failure(ErrorCode::InvalidArgument, "migration generation is unset");
  if (schema.empty()) return Status::failure(ErrorCode::InvalidArgument, "migration schema id is empty");
  if (!source.is_set() || !target.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "migration source and target generations must be set");
  }
  if (source == target) {
    return Status::failure(ErrorCode::InvalidArgument, "migration source equals target generation");
  }
  if (!runtime_generation.is_set()) {
    return Status::failure(ErrorCode::InvalidArgument, "migration must bind a runtime generation");
  }
  if (!epoch.is_set()) return Status::failure(ErrorCode::InvalidArgument, "migration must bind an evolution epoch");
  if (irreversible && !irreversible_acknowledged) {
    return Status::failure(ErrorCode::Unauthorized, "irreversible migration was not explicitly acknowledged");
  }
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Real transforms
// ---------------------------------------------------------------------------
bool StateMigrator::transform_supported(const SchemaId& schema, SchemaGeneration from,
                                        SchemaGeneration to) noexcept {
  if (schema != component_state_schema()) return false;
  const std::uint64_t pair = (from.raw() << 8) | to.raw();
  switch (pair) {
    case ((1ull << 8) | 2ull):  // v1 -> v2: add required state_epoch, add region label
    case ((2ull << 8) | 1ull):  // v2 -> v1: drop state_epoch (reversible)
    case ((2ull << 8) | 3ull):  // v2 -> v3: replace legacy_token with token_digest (irreversible)
      return true;
    default:
      return false;
  }
}

bool StateMigrator::transform_is_irreversible(const SchemaId& schema, SchemaGeneration from,
                                              SchemaGeneration to) noexcept {
  if (schema != component_state_schema()) return false;
  return from.raw() == 2 && to.raw() == 3;
}

std::string StateMigrator::checkpoint_path(const std::string& path) { return path + ".checkpoint"; }
std::string StateMigrator::staging_path(const std::string& path) { return path + ".migrating"; }

namespace {

const FieldId& field_epoch() {
  static const FieldId id = FieldId::from_valid("state_epoch");
  return id;
}
const FieldId& field_token() {
  static const FieldId id = FieldId::from_valid("legacy_token");
  return id;
}
const FieldId& field_token_digest() {
  static const FieldId id = FieldId::from_valid("token_digest");
  return id;
}

Status apply_transform(const StateObject& source, const MigrationContract& contract, StateObject& out) {
  out = source;
  const std::uint64_t pair = (contract.source.raw() << 8) | contract.target.raw();
  switch (pair) {
    case ((1ull << 8) | 2ull): {
      const std::uint64_t epoch_value =
          contract.epoch.is_set() ? contract.epoch.raw() : 1ull;
      const Status status = out.set_u64(field_epoch(), epoch_value);
      if (status.is_failure()) return status;
      return Status::ok();
    }
    case ((2ull << 8) | 1ull): {
      const StateValue* epoch = out.find(field_epoch());
      if (epoch == nullptr) {
        return Status::failure(ErrorCode::Corrupt, "source state does not carry the migration-epoch field");
      }
      return out.remove_field(field_epoch());
    }
    case ((2ull << 8) | 3ull): {
      const StateValue* token = out.find(field_token());
      if (token == nullptr) {
        return Status::failure(ErrorCode::Corrupt, "source state does not carry the legacy token field");
      }
      const IntegrityDigest derived = IntegrityDigest::of(token->text);
      const Status removed = out.remove_field(field_token());
      if (removed.is_failure()) return removed;
      return out.set_digest(field_token_digest(), derived);
    }
    default:
      return Status::failure(ErrorCode::Unsupported, "no migration transform exists for this generation pair");
  }
}

std::string_view detail_of(const Status& status) { return status.detail(); }

}  // namespace

// ---------------------------------------------------------------------------
// StateMigrator
// ---------------------------------------------------------------------------
MigrationResult StateMigrator::inspect(const std::string& path) const {
  MigrationResult result;
  if (!file_exists(path)) {
    result.status = Status::failure(ErrorCode::NotFound, "state file is absent");
    result.outcome = MigrationOutcome::RevalidationRequired;
    result.detail = DetailText::from_valid("state file is absent");
    return result;
  }
  StateFile file;
  const Status read = read_state_file(path, file);
  if (read.is_failure()) {
    result.status = read;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("state file could not be decoded");
    return result;
  }
  result.status = Status::ok();
  result.outcome = MigrationOutcome::Ready;
  result.observed_generation = file.header.generation;
  result.source_digest = file.header.payload_digest;
  result.rollback_metadata_present = file_exists(checkpoint_path(path));
  result.detail = DetailText::from_valid("state file decoded and integrity verified");
  return result;
}

MigrationResult StateMigrator::plan_migration(const std::string& path,
                                              const MigrationContract& contract) const {
  MigrationResult result;
  const Status contract_status = contract.validate();
  if (contract_status.is_failure()) {
    result.status = contract_status;
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("migration contract is invalid");
    return result;
  }
  if (contract.source == contract.target) {
    result.status = Status::ok();
    result.outcome = MigrationOutcome::NotRequired;
    result.detail = DetailText::from_valid("source and target schema generations are identical");
    return result;
  }
  if (schemas_ == nullptr) {
    result.status = Status::failure(ErrorCode::Internal, "migrator has no schema registry");
    result.outcome = MigrationOutcome::Failed;
    return result;
  }
  const SchemaDescriptor* source_descriptor = schemas_->find(contract.schema, contract.source);
  const SchemaDescriptor* target_descriptor = schemas_->find(contract.schema, contract.target);
  if (source_descriptor == nullptr || target_descriptor == nullptr) {
    result.status = Status::failure(ErrorCode::NotFound, "migration references an unregistered schema generation");
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("schema generation is not registered");
    return result;
  }
  if (!transform_supported(contract.schema, contract.source, contract.target)) {
    result.status = Status::failure(ErrorCode::Unsupported, "no migration function for this generation pair");
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("migration function unavailable");
    return result;
  }
  // Irreversibility is a property of the transition, not of the target: a
  // migration is reversible exactly when a transform exists that can take the
  // target generation back to the source generation.
  const bool irreversible =
      contract.irreversible || transform_is_irreversible(contract.schema, contract.source, contract.target) ||
      !transform_supported(contract.schema, contract.target, contract.source) ||
      target_descriptor->has_irreversible_field();
  if (irreversible && !contract.irreversible_acknowledged) {
    result.status = Status::failure(ErrorCode::Unauthorized,
                                    "irreversible migration requires explicit acknowledgement");
    result.outcome = MigrationOutcome::Blocked;
    result.irreversible = true;
    result.detail = DetailText::from_valid("irreversible migration not acknowledged");
    return result;
  }
  StateFile file;
  const Status read = read_state_file(path, file);
  if (read.is_failure()) {
    result.status = read;
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("state file could not be read");
    return result;
  }
  if (file.header.schema != contract.schema || file.header.generation != contract.source) {
    result.status = Status::failure(ErrorCode::StaleGeneration, "state file is not at the declared source generation");
    result.outcome = MigrationOutcome::RevalidationRequired;
    result.observed_generation = file.header.generation;
    result.detail = DetailText::from_valid("source generation mismatch");
    return result;
  }
  StateObject object;
  const Status decoded = StateObject::decode(file.payload, object);
  if (decoded.is_failure()) {
    result.status = decoded;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("state payload is not decodable");
    return result;
  }
  const Status source_valid = object.validate_against(*source_descriptor);
  if (source_valid.is_failure()) {
    result.status = source_valid;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("authoritative state does not satisfy its own schema");
    return result;
  }
  StateObject target_object;
  const Status transformed = apply_transform(object, contract, target_object);
  if (transformed.is_failure()) {
    result.status = transformed;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("migration transform failed");
    return result;
  }
  const Status target_valid = target_object.validate_against(*target_descriptor);
  if (target_valid.is_failure()) {
    result.status = target_valid;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("migrated state does not satisfy the target schema");
    return result;
  }
  result.status = Status::ok();
  result.outcome = MigrationOutcome::Ready;
  result.observed_generation = file.header.generation;
  result.source_digest = file.header.payload_digest;
  result.target_digest = IntegrityDigest::of(target_object.encode());
  result.irreversible = irreversible;
  result.rollback_metadata_present = file_exists(checkpoint_path(path));
  result.detail = DetailText::from_valid("migration validated; authoritative state untouched");
  return result;
}

MigrationResult StateMigrator::migrate(const std::string& path, const MigrationContract& contract) const {
  const MigrationResult planned = plan_migration(path, contract);
  if (planned.outcome == MigrationOutcome::NotRequired) return planned;
  if (planned.status.is_failure()) return planned;

  StateFile source_file;
  const Status read = read_state_file(path, source_file);
  if (read.is_failure()) {
    MigrationResult result;
    result.status = read;
    result.outcome = MigrationOutcome::Failed;
    return result;
  }

  StateObject object;
  StateObject target_object;
  if (StateObject::decode(source_file.payload, object).is_failure()) {
    MigrationResult result;
    result.status = Status::failure(ErrorCode::Corrupt, "state payload is not decodable");
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("state payload is not decodable");
    return result;
  }
  const Status transformed = apply_transform(object, contract, target_object);
  if (transformed.is_failure()) {
    MigrationResult result;
    result.status = transformed;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("migration transform failed");
    return result;
  }

  StateFile target_file;
  target_file.header.format = source_file.header.format;
  target_file.header.schema = contract.schema;
  target_file.header.generation = contract.target;
  target_file.header.migration = contract.generation;
  target_file.header.epoch = contract.epoch;
  target_file.header.policy = contract.policy;
  target_file.payload = target_object.encode();

  const std::string staging = staging_path(path);
  const Status staged = write_state_file(staging, target_file);
  if (staged.is_failure()) {
    MigrationResult result;
    result.status = staged;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("staged migration state could not be written");
    return result;
  }

  // Verify the staged file by decoding it again before it becomes authoritative.
  StateFile verification;
  const Status verified = read_state_file(staging, verification);
  if (verified.is_failure() || verification.header.generation != contract.target) {
    (void)remove_file_if_exists(staging);
    MigrationResult result;
    result.status = verified.is_failure() ? verified
                                          : Status::failure(ErrorCode::Corrupt,
                                                            "staged migration state failed verification");
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("staged state failed verification; authoritative state untouched");
    return result;
  }

  const bool preserve = contract.preserve_rollback_metadata;
  if (preserve) {
    std::string original;
    const Status original_read = read_file_bounded(path, limits::kStateFileBytes, original);
    if (original_read.is_failure()) {
      (void)remove_file_if_exists(staging);
      MigrationResult result;
      result.status = original_read;
      result.outcome = MigrationOutcome::Failed;
      result.detail = DetailText::from_valid("rollback metadata could not be preserved");
      return result;
    }
    const Status written = write_file_atomic(checkpoint_path(path), original);
    if (written.is_failure()) {
      (void)remove_file_if_exists(staging);
      MigrationResult result;
      result.status = written;
      result.outcome = MigrationOutcome::Failed;
      result.detail = DetailText::from_valid("rollback metadata could not be preserved");
      return result;
    }
  }

  const Status committed = rename_file(staging, path);
  if (committed.is_failure()) {
    (void)remove_file_if_exists(staging);
    MigrationResult result;
    result.status = committed;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("atomic commit of migrated state failed");
    return result;
  }

  MigrationResult result;
  result.status = Status::ok();
  result.observed_generation = contract.target;
  result.source_digest = source_file.header.payload_digest;
  result.target_digest = target_file.header.payload_digest;
  result.rollback_metadata_present = preserve && file_exists(checkpoint_path(path));
  const bool reversible =
      !planned.irreversible && transform_supported(contract.schema, contract.target, contract.source);
  result.rollback_available = reversible && result.rollback_metadata_present;
  result.irreversible = planned.irreversible;
  if (planned.irreversible) {
    result.outcome = MigrationOutcome::Irreversible;
    result.detail = DetailText::from_valid("irreversible migration committed; rollback no longer available");
  } else if (result.rollback_available) {
    result.outcome = MigrationOutcome::RollbackAvailable;
    result.detail = DetailText::from_valid("migration committed with verified rollback metadata");
  } else {
    result.outcome = MigrationOutcome::Committed;
    result.detail = DetailText::from_valid("migration committed");
  }
  return result;
}

MigrationResult StateMigrator::reverse(const std::string& path, const MigrationContract& contract) const {
  MigrationResult result;
  // A reverse migration is the mirror contract: target -> source.
  MigrationContract reverse_contract = contract;
  reverse_contract.source = contract.target;
  reverse_contract.target = contract.source;
  reverse_contract.irreversible = false;
  reverse_contract.irreversible_acknowledged = false;

  const Status contract_status = reverse_contract.validate();
  if (contract_status.is_failure()) {
    result.status = contract_status;
    result.outcome = MigrationOutcome::Blocked;
    return result;
  }
  if (!transform_supported(reverse_contract.schema, reverse_contract.source, reverse_contract.target)) {
    result.status = Status::failure(ErrorCode::Unsupported, "no reverse migration function exists");
    result.outcome = MigrationOutcome::Irreversible;
    result.irreversible = true;
    result.detail = DetailText::from_valid("rollback barrier: reverse migration unavailable");
    return result;
  }
  const std::string checkpoint = checkpoint_path(path);
  if (!file_exists(checkpoint)) {
    result.status = Status::failure(ErrorCode::NotFound, "rollback metadata is absent");
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("rollback metadata is absent");
    return result;
  }
  std::string original;
  const Status read = read_file_bounded(checkpoint, limits::kStateFileBytes, original);
  if (read.is_failure()) {
    result.status = read;
    result.outcome = MigrationOutcome::Failed;
    return result;
  }
  StateFile file;
  const Status decoded = decode_state_file(original, file);
  if (decoded.is_failure()) {
    result.status = decoded;
    result.outcome = MigrationOutcome::Failed;
    result.detail = DetailText::from_valid("rollback metadata is corrupt");
    return result;
  }
  if (file.header.generation != reverse_contract.target) {
    result.status = Status::failure(ErrorCode::Conflict, "rollback metadata generation does not match the target");
    result.outcome = MigrationOutcome::Blocked;
    result.detail = DetailText::from_valid("rollback metadata generation mismatch");
    return result;
  }
  const Status restored = write_file_atomic(path, original);
  if (restored.is_failure()) {
    result.status = restored;
    result.outcome = MigrationOutcome::Failed;
    return result;
  }
  result.status = Status::ok();
  result.outcome = MigrationOutcome::Committed;
  result.observed_generation = reverse_contract.target;
  result.target_digest = file.header.payload_digest;
  result.rollback_metadata_present = true;
  result.rollback_available = true;
  result.detail = DetailText::from_valid("state restored from rollback metadata");
  return result;
}

MigrationResult StateMigrator::reconcile(const std::string& path, const MigrationContract& contract) const {
  MigrationResult result;
  const std::string staging = staging_path(path);
  const bool staging_present = file_exists(staging);
  const bool state_present = file_exists(path);

  if (!state_present && !staging_present) {
    result.status = Status::failure(ErrorCode::NotFound, "neither authoritative state nor staging state exists");
    result.outcome = MigrationOutcome::OutcomeUnknown;
    result.detail = DetailText::from_valid("no state file to reconcile");
    return result;
  }
  if (!state_present && staging_present) {
    result.status = Status::failure(ErrorCode::Conflict, "staging state exists without authoritative state");
    result.outcome = MigrationOutcome::RevalidationRequired;
    result.detail = DetailText::from_valid("staging state persists; authoritative state is absent");
    return result;
  }

  StateFile file;
  const Status read = read_state_file(path, file);
  if (read.is_failure()) {
    result.status = read;
    result.outcome = MigrationOutcome::OutcomeUnknown;
    result.detail = DetailText::from_valid("authoritative state is not decodable");
    return result;
  }
  result.observed_generation = file.header.generation;
  result.source_digest = file.header.payload_digest;

  if (file.header.generation == contract.target) {
    // Conservative: the commit is visible, so the migration is treated as
    // committed exactly once, and a leftover staging file is cleaned up.
    if (staging_present) (void)remove_file_if_exists(staging);
    result.status = Status::ok();
    result.outcome = MigrationOutcome::Committed;
    result.irreversible = transform_is_irreversible(contract.schema, contract.source, contract.target);
    result.rollback_metadata_present = file_exists(checkpoint_path(path));
    result.rollback_available = transform_supported(contract.schema, contract.target, contract.source) &&
                                result.rollback_metadata_present && !result.irreversible;
    result.detail = DetailText::from_valid("authoritative state already carries the target generation");
    return result;
  }
  if (file.header.generation == contract.source) {
    result.status = Status::ok();
    result.outcome = staging_present ? MigrationOutcome::RevalidationRequired : MigrationOutcome::NotRequired;
    result.detail = DetailText::from_valid(
        staging_present ? "staging state present but not committed; operator decision required"
                        : "authoritative state is still at the source generation");
    return result;
  }
  result.status = Status::failure(ErrorCode::Conflict, "authoritative state is at an unexpected generation");
  result.outcome = MigrationOutcome::OutcomeUnknown;
  result.detail = DetailText::from_valid("authoritative state generation is neither source nor target");
  return result;
}

}  // namespace ref
