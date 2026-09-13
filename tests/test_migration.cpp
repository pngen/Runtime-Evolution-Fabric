// Runtime Evolution Fabric - state migration, rollback and irreversible barrier.
#include <fstream>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace ref;
using reftest::TempDir;

namespace {

MigrationContract contract_for(std::uint64_t source, std::uint64_t target, bool irreversible = false) {
  MigrationContract contract;
  contract.id = MigrationId::from_valid("migration-" + std::to_string(source) + "-" + std::to_string(target));
  contract.generation = MigrationGeneration::from_raw(1);
  contract.schema = SchemaId::from_valid("component-state");
  contract.source = SchemaGeneration::from_raw(source);
  contract.target = SchemaGeneration::from_raw(target);
  contract.runtime_generation = RuntimeGeneration::from_raw(2);
  contract.epoch = EvolutionEpoch::from_raw(3);
  contract.policy = PolicyGeneration::from_raw(1);
  contract.plan = EvolutionPlanId::from_valid("plan-1");
  contract.irreversible = irreversible;
  contract.irreversible_acknowledged = irreversible;
  return contract;
}

std::string read_all(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return bytes;
}

}  // namespace

REF_TEST(migration, state_object_round_trips_canonically) {
  StateObject object;
  REF_CHECK_STATUS_OK(object.set_text(FieldId::from_valid("owner"), "orders"));
  REF_CHECK_STATUS_OK(object.set_u64(FieldId::from_valid("mutation_counter"), 7));
  REF_CHECK_STATUS_OK(object.set_bool(FieldId::from_valid("ready"), true));
  REF_CHECK_STATUS_OK(object.set_digest(FieldId::from_valid("token_digest"),
                                        IntegrityDigest::of("token")));
  StateObject other;
  REF_CHECK_STATUS_OK(other.set_digest(FieldId::from_valid("token_digest"),
                                       IntegrityDigest::of("token")));
  REF_CHECK_STATUS_OK(other.set_bool(FieldId::from_valid("ready"), true));
  REF_CHECK_STATUS_OK(other.set_u64(FieldId::from_valid("mutation_counter"), 7));
  REF_CHECK_STATUS_OK(other.set_text(FieldId::from_valid("owner"), "orders"));
  REF_CHECK_EQ(object.encode(), other.encode());  // canonical order is insertion independent
  StateObject decoded;
  REF_CHECK_STATUS_OK(StateObject::decode(object.encode(), decoded));
  REF_CHECK_EQ(decoded.size(), object.size());
  REF_CHECK_STATUS_CODE(StateObject::decode(object.encode() + "junk", decoded), ErrorCode::Corrupt);
  REF_CHECK_STATUS_CODE(StateObject::decode("", decoded), ErrorCode::Truncated);
}

REF_TEST(migration, read_and_write_asymmetry_is_enforced) {
  SchemaRegistry registry = reftest::make_schema_registry();
  const SchemaId id = SchemaId::from_valid("component-state");
  const SchemaDescriptor* v1 = registry.find(id, SchemaGeneration::from_raw(1));
  const SchemaDescriptor* v3 = registry.find(id, SchemaGeneration::from_raw(3));
  REF_CHECK(v1 != nullptr && v3 != nullptr);
  StateObject v1_state;
  REF_CHECK_STATUS_OK(v1_state.set_text(FieldId::from_valid("owner"), "orders"));
  REF_CHECK_STATUS_OK(v1_state.set_u64(FieldId::from_valid("mutation_counter"), 1));
  REF_CHECK_STATUS_OK(v1_state.set_text(FieldId::from_valid("legacy_token"), "token-orders"));
  REF_CHECK_STATUS_OK(v1_state.validate_against(*v1));
  // A v1 object is not valid under v3, and a v3 object is not valid under v1.
  REF_CHECK_STATUS_CODE(v1_state.validate_against(*v3), ErrorCode::Corrupt);
  StateObject v3_state;
  REF_CHECK_STATUS_OK(v3_state.set_text(FieldId::from_valid("owner"), "orders"));
  REF_CHECK_STATUS_OK(v3_state.set_u64(FieldId::from_valid("mutation_counter"), 1));
  REF_CHECK_STATUS_OK(v3_state.set_u64(FieldId::from_valid("state_epoch"), 4));
  REF_CHECK_STATUS_OK(v3_state.set_digest(FieldId::from_valid("token_digest"),
                                          IntegrityDigest::of("token-orders")));
  REF_CHECK_STATUS_OK(v3_state.validate_against(*v3));
  REF_CHECK_STATUS_CODE(v3_state.validate_against(*v1), ErrorCode::Corrupt);
}

REF_TEST(migration, transactional_migration_on_a_real_file) {
  TempDir dir("migration");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 3));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);

  const MigrationResult inspection = migrator.inspect(path);
  REF_CHECK_STATUS_OK(inspection.status);
  REF_CHECK_EQ(inspection.observed_generation.raw(), 1ull);

  const MigrationContract contract = contract_for(1, 2);
  const MigrationResult planned = migrator.plan_migration(path, contract);
  REF_CHECK_STATUS_OK(planned.status);
  REF_CHECK_OUTCOME(planned.outcome, MigrationOutcome::Ready);
  // Planning must not touch authoritative state.
  StateFile before;
  REF_CHECK_STATUS_OK(read_state_file(path, before));
  REF_CHECK_EQ(before.header.generation.raw(), 1ull);

  const MigrationResult committed = migrator.migrate(path, contract);
  REF_CHECK_STATUS_OK(committed.status);
  REF_CHECK_OUTCOME(committed.outcome, MigrationOutcome::RollbackAvailable);
  REF_CHECK(committed.rollback_metadata_present);
  REF_CHECK(committed.rollback_available);
  REF_CHECK(file_exists(StateMigrator::checkpoint_path(path)));

  StateFile after;
  REF_CHECK_STATUS_OK(read_state_file(path, after));
  REF_CHECK_EQ(after.header.generation.raw(), 2ull);
  REF_CHECK_EQ(after.header.migration.raw(), 1ull);
  REF_CHECK_EQ(after.header.epoch.raw(), 3ull);
  StateObject migrated;
  REF_CHECK_STATUS_OK(StateObject::decode(after.payload, migrated));
  const StateValue* epoch = migrated.find(FieldId::from_valid("state_epoch"));
  REF_CHECK(epoch != nullptr);
  REF_CHECK_EQ(epoch->number, 3ull);
  // No staging debris is left behind.
  REF_CHECK(!file_exists(StateMigrator::staging_path(path)));

  // Rollback restores exactly the original bytes.
  const std::string original = read_all(StateMigrator::checkpoint_path(path));
  const MigrationResult reversed = migrator.reverse(path, contract);
  REF_CHECK_STATUS_OK(reversed.status);
  REF_CHECK_OUTCOME(reversed.outcome, MigrationOutcome::Committed);
  StateFile restored;
  REF_CHECK_STATUS_OK(read_state_file(path, restored));
  REF_CHECK_EQ(restored.header.generation.raw(), 1ull);
  REF_CHECK_EQ(restored.payload, before.payload);
  REF_CHECK(!original.empty());
}

REF_TEST(migration, irreversible_barrier_removes_rollback) {
  TempDir dir("irreversible");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 5));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);

  // 1 -> 2 is reversible, 2 -> 3 is not.
  REF_CHECK_STATUS_OK(migrator.migrate(path, contract_for(1, 2)).status);
  const MigrationContract to_v3 = contract_for(2, 3, true);
  const MigrationResult irreversible = migrator.migrate(path, to_v3);
  REF_CHECK_STATUS_OK(irreversible.status);
  REF_CHECK_OUTCOME(irreversible.outcome, MigrationOutcome::Irreversible);
  REF_CHECK(irreversible.irreversible);
  REF_CHECK(!irreversible.rollback_available);

  StateFile file;
  REF_CHECK_STATUS_OK(read_state_file(path, file));
  REF_CHECK_EQ(file.header.generation.raw(), 3ull);
  StateObject object;
  REF_CHECK_STATUS_OK(StateObject::decode(file.payload, object));
  REF_CHECK(object.find(FieldId::from_valid("legacy_token")) == nullptr);
  const StateValue* digest = object.find(FieldId::from_valid("token_digest"));
  REF_CHECK(digest != nullptr);
  REF_CHECK(digest->digest == IntegrityDigest::of("token-orders"));

  // The reverse migration no longer exists, so rollback must be refused.
  const MigrationResult reverse = migrator.reverse(path, to_v3);
  REF_CHECK_STATUS_CODE(reverse.status, ErrorCode::Unsupported);
  REF_CHECK_OUTCOME(reverse.outcome, MigrationOutcome::Irreversible);

  // The barrier is also visible in the schema registry.
  REF_CHECK(!registry.has_reverse_migration(SchemaId::from_valid("component-state"),
                                             SchemaGeneration::from_raw(3)));
}

REF_TEST(migration, unacknowledged_irreversible_migration_is_blocked) {
  TempDir dir("ack");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 1));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);
  REF_CHECK_STATUS_OK(migrator.migrate(path, contract_for(1, 2)).status);
  MigrationContract contract = contract_for(2, 3, true);
  contract.irreversible_acknowledged = false;
  const MigrationResult result = migrator.migrate(path, contract);
  REF_CHECK_STATUS_CODE(result.status, ErrorCode::Unauthorized);
  REF_CHECK_OUTCOME(result.outcome, MigrationOutcome::Blocked);
  StateFile file;
  REF_CHECK_STATUS_OK(read_state_file(path, file));
  REF_CHECK_EQ(file.header.generation.raw(), 2ull);  // authoritative state untouched
}

REF_TEST(migration, failed_migration_leaves_authoritative_state_unchanged) {
  TempDir dir("failed");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 2));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);
  const std::string before = read_all(path);
  // No migration function exists for 1 -> 3.
  const MigrationResult blocked = migrator.migrate(path, contract_for(1, 3));
  REF_CHECK_STATUS_CODE(blocked.status, ErrorCode::Unsupported);
  REF_CHECK_OUTCOME(blocked.outcome, MigrationOutcome::Blocked);
  REF_CHECK_EQ(read_all(path), before);
  REF_CHECK(!file_exists(StateMigrator::staging_path(path)));
  // A source generation mismatch is a revalidation problem, not a silent fix.
  const MigrationResult mismatch = migrator.migrate(path, contract_for(2, 1));
  REF_CHECK_STATUS_CODE(mismatch.status, ErrorCode::StaleGeneration);
  REF_CHECK_OUTCOME(mismatch.outcome, MigrationOutcome::RevalidationRequired);
  REF_CHECK_EQ(read_all(path), before);
}

REF_TEST(migration, ambiguous_completion_is_reconciled_without_double_commit) {
  TempDir dir("reconcile");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 4));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);
  const MigrationContract contract = contract_for(1, 2);
  REF_CHECK_STATUS_OK(migrator.migrate(path, contract).status);

  // The commit is visible: reconciliation reports it committed, exactly once,
  // and cleans up any staging debris left by a crashed attempt.
  REF_CHECK_STATUS_OK(write_file_atomic(StateMigrator::staging_path(path), "staged but uncommitted"));
  const MigrationResult first = migrator.reconcile(path, contract);
  REF_CHECK_STATUS_OK(first.status);
  REF_CHECK_OUTCOME(first.outcome, MigrationOutcome::Committed);
  REF_CHECK(!file_exists(StateMigrator::staging_path(path)));
  const MigrationResult second = migrator.reconcile(path, contract);
  REF_CHECK_STATUS_OK(second.status);
  REF_CHECK_OUTCOME(second.outcome, MigrationOutcome::Committed);

  // Nothing committed yet: the source generation is still authoritative.
  const MigrationResult untouched = migrator.reconcile(path, contract_for(2, 3, true));
  REF_CHECK_STATUS_OK(untouched.status);
  REF_CHECK_OUTCOME(untouched.outcome, MigrationOutcome::NotRequired);

  // An unexpected generation is never guessed at.
  StateFile odd;
  REF_CHECK_STATUS_OK(read_state_file(path, odd));
  odd.header.generation = SchemaGeneration::from_raw(9);
  REF_CHECK_STATUS_OK(write_state_file(path, odd));
  const MigrationResult confused = migrator.reconcile(path, contract);
  REF_CHECK(confused.outcome == MigrationOutcome::OutcomeUnknown ||
            confused.outcome == MigrationOutcome::RevalidationRequired);
  REF_CHECK_STATUS_CODE(confused.status, ErrorCode::Conflict);
}

REF_TEST(migration, corrupted_state_is_rejected_before_mutation) {
  TempDir dir("corrupt");
  const std::string path = dir.file("worker.state");
  REF_CHECK_STATUS_OK(reftest::make_worker_state(path, "orders", 1));
  SchemaRegistry registry = reftest::make_schema_registry();
  StateMigrator migrator(&registry);
  std::string bytes = read_all(path);
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0xFF);
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const MigrationResult result = migrator.migrate(path, contract_for(1, 2));
  REF_CHECK(result.status.is_failure());
  REF_CHECK(result.outcome == MigrationOutcome::Failed || result.outcome == MigrationOutcome::Blocked);
  REF_CHECK(!file_exists(StateMigrator::checkpoint_path(path)));
}
