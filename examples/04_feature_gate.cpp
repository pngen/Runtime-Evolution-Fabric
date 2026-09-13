// Example 4: a feature gate that stays closed until its requirements hold.
#include <cstdio>

#include "ref/ref.hpp"

using namespace ref;

int main() {
  FeatureGateRegistry gates;
  FeatureGate transform;
  transform.id = FeatureGateId::from_valid("state-token-transform");
  transform.generation = FeatureGateGeneration::first();
  transform.min_runtime_generation = RuntimeGeneration::from_raw(2);
  transform.min_protocol_generation = ProtocolGeneration::from_raw(2);
  transform.min_schema_generation = SchemaGeneration::from_raw(3);
  transform.required_peer_outcome = CompatOutcome::FullyCompatible;
  transform.provenance = EvidenceClass::Real;
  transform.evidence = EvidenceGeneration::first();
  (void)gates.define(transform);

  GateContext context;
  context.runtime_generation = RuntimeGeneration::from_raw(2);
  context.protocol_generation = ProtocolGeneration::from_raw(1);  // mixed-version phase
  context.schema_generation = SchemaGeneration::from_raw(2);
  context.peer_outcome = CompatOutcome::MixedVersionCompatible;
  context.cohort = CohortId::from_valid("canary-a");

  GateDecision closed = gates.evaluate(transform.id, context);
  std::printf("during mixed-version operation: permitted=%s (%s)\n", closed.permitted ? "yes" : "no",
              closed.status.to_string().c_str());

  (void)gates.set_enabled(transform.id, true, EvolutionPlanId::from_valid("orders-upgrade"),
                          StageGeneration::from_raw(9));
  closed = gates.evaluate(transform.id, context);
  std::printf("after enablement, still below the protocol requirement: permitted=%s (%s)\n",
              closed.permitted ? "yes" : "no", closed.status.to_string().c_str());

  context.protocol_generation = ProtocolGeneration::from_raw(2);
  context.schema_generation = SchemaGeneration::from_raw(3);
  context.peer_outcome = CompatOutcome::FullyCompatible;
  const GateDecision open = gates.evaluate(transform.id, context);
  std::printf("once drained and renegotiated: permitted=%s (%s)\n", open.permitted ? "yes" : "no",
              open.status.to_string().c_str());
  std::printf("gate generation is now %llu\n",
              static_cast<unsigned long long>(gates.find(transform.id)->generation.raw()));
  return 0;
}
