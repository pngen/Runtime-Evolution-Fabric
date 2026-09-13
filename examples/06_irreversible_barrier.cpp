// Example 6: an irreversible migration barrier.
#include <cstdio>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  SchemaRegistry registry;
  (void)install_builtin_state_schemas(registry);
  const SchemaId schema = SchemaId::from_valid("component-state");

  const std::string path = "example-06.state";
  StateObject object;
  (void)object.set_text(FieldId::from_valid("owner"), "orders");
  (void)object.set_u64(FieldId::from_valid("mutation_counter"), 3);
  (void)object.set_text(FieldId::from_valid("legacy_token"), "token-orders");
  StateFile file;
  file.header.format = kStateFormatGenerationV1;
  file.header.schema = schema;
  file.header.generation = SchemaGeneration::from_raw(1);
  file.header.epoch = EvolutionEpoch::from_raw(1);
  file.payload = object.encode();
  (void)write_state_file(path, file);

  StateMigrator migrator(&registry);
  MigrationContract contract;
  contract.id = MigrationId::from_valid("migration-2-3");
  contract.generation = MigrationGeneration::from_raw(1);
  contract.schema = schema;
  contract.source = SchemaGeneration::from_raw(2);
  contract.target = SchemaGeneration::from_raw(3);
  contract.runtime_generation = RuntimeGeneration::from_raw(2);
  contract.epoch = EvolutionEpoch::from_raw(2);
  contract.plan = EvolutionPlanId::from_valid("orders-upgrade");
  contract.irreversible = true;

  // An irreversible migration is refused unless it is explicitly acknowledged.
  const MigrationResult refused = migrator.migrate(path, contract);
  std::printf("irreversible without acknowledgement: %s / %s\n", refused.status.to_string().c_str(),
              to_string(refused.outcome));

  // Walk 1 -> 2 first, then cross the barrier with acknowledgement.
  MigrationContract reversible = contract;
  reversible.source = SchemaGeneration::from_raw(1);
  reversible.target = SchemaGeneration::from_raw(2);
  reversible.irreversible = false;
  (void)migrator.migrate(path, reversible);

  contract.irreversible_acknowledged = true;
  const MigrationResult crossed = migrator.migrate(path, contract);
  std::printf("crossing the barrier: %s / %s (rollback available: %s)\n",
              crossed.status.to_string().c_str(), to_string(crossed.outcome),
              crossed.rollback_available ? "yes" : "no");

  const MigrationResult rollback = migrator.reverse(path, contract);
  std::printf("attempting rollback afterwards: %s / %s\n", rollback.status.to_string().c_str(),
              to_string(rollback.outcome));
  return 0;
}
