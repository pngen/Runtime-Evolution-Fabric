// Example 3: schema migration with read/write asymmetry and a real file.
//
// Shows: a v1 state file, v1 readable by v2, v2 not readable by v1, a
// transactional migration to v2, and the rollback path that stays available.
#include <cstdio>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  SchemaRegistry registry;
  (void)install_builtin_state_schemas(registry);

  const SchemaId schema = SchemaId::from_valid("component-state");
  const SchemaCompatResult v2_reads_v1 =
      registry.read_compatibility(schema, SchemaGeneration::from_raw(2), SchemaGeneration::from_raw(1));
  const SchemaCompatResult v1_reads_v2 =
      registry.read_compatibility(schema, SchemaGeneration::from_raw(1), SchemaGeneration::from_raw(2));
  std::printf("v2 reads v1 state: %s\n", to_string(v2_reads_v1.outcome));
  std::printf("v1 reads v2 state: %s\n", to_string(v1_reads_v2.outcome));

  const std::string path = "example-03.state";
  StateObject object;
  (void)object.set_text(FieldId::from_valid("owner"), "orders");
  (void)object.set_u64(FieldId::from_valid("mutation_counter"), 12);
  (void)object.set_text(FieldId::from_valid("legacy_token"), "token-orders");
  StateFile file;
  file.header.format = kStateFormatGenerationV1;
  file.header.schema = schema;
  file.header.generation = SchemaGeneration::from_raw(1);
  file.header.epoch = EvolutionEpoch::from_raw(1);
  file.payload = object.encode();
  std::printf("write v1 state: %s\n", write_state_file(path, file).to_string().c_str());

  StateMigrator migrator(&registry);
  MigrationContract contract;
  contract.id = MigrationId::from_valid("migration-1-2");
  contract.generation = MigrationGeneration::from_raw(1);
  contract.schema = schema;
  contract.source = SchemaGeneration::from_raw(1);
  contract.target = SchemaGeneration::from_raw(2);
  contract.runtime_generation = RuntimeGeneration::from_raw(2);
  contract.epoch = EvolutionEpoch::from_raw(1);
  contract.plan = EvolutionPlanId::from_valid("orders-upgrade");

  const MigrationResult committed = migrator.migrate(path, contract);
  std::printf("migrate: %s / %s\n", committed.status.to_string().c_str(), to_string(committed.outcome));
  StateFile migrated;
  (void)read_state_file(path, migrated);
  std::printf("state schema generation is now %llu\n",
              static_cast<unsigned long long>(migrated.header.generation.raw()));

  const MigrationResult reversed = migrator.reverse(path, contract);
  std::printf("rollback: %s / %s\n", reversed.status.to_string().c_str(), to_string(reversed.outcome));
  StateFile restored;
  (void)read_state_file(path, restored);
  std::printf("state schema generation after rollback is %llu\n",
              static_cast<unsigned long long>(restored.header.generation.raw()));
  return 0;
}
