// Runtime Evolution Fabric - inspection CLI.
//
// Read-only inspection is the default. Administrative mutation lives behind the
// explicit "admin" verb and is never mixed into read paths.
//
//   refcli --state <file> show <kind>        inspect durable state offline
//   refcli --state <file> verify             verify durable state integrity
//   refcli --host <h> --port <p> show <kind> inspect a live coordinator
//   refcli --host <h> --port <p> admin <cmd> administrative mutation
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "cli_arguments.hpp"
#include "ref/ref.hpp"

namespace {

void print_usage() {
  std::printf(
      "Runtime Evolution Fabric %s inspection CLI\n"
      "\n"
      "Read-only:\n"
      "  refcli --tool-version\n"
      "  refcli --state <file> verify\n"
      "  refcli --state <file> show <components|plans|compatibility|protocols|schemas|gates|migrations|\n"
      "                               workers|epochs|retirement|snapshots|reconcile|authority>\n"
      "  refcli --host <h> --port <p> show <kind> [--component <id>]\n"
      "  refcli --host <h> --port <p> explain [--component <id>]\n"
      "\n"
      "Administrative (mutation):\n"
      "  refcli --host <h> --port <p> admin publish-component --component <id> --generation <n>\n"
      "        --version <v> --protocols <csv> --schemas <csv> [--lifecycle CURRENT]\n"
      "  refcli --host <h> --port <p> admin publish-compatibility --from <n> --to <n>\n"
      "        [--profile compatible|mixed-version] [--aspect-<aspect> <OUTCOME>] [--control-channel ...]\n"
      "  refcli --host <h> --port <p> admin plan --component <id> --candidate <n>\n"
      "        [--canary <cohort>] [--cohort <cohort>] [--migration <id>:<gen>:<from>:<to>]\n"
      "        [--require-migration-barrier] [--require-canary 0|1] [--require-mixed-version 0|1]\n"
      "        [--require-new-writer-barrier 0|1] [--require-old-writer-drain 0|1]\n"
      "  refcli --host <h> --port <p> admin advance --plan <id> --stage <STAGE> [--stage-generation <n>]\n"
      "  refcli --host <h> --port <p> admin rebind --plan <id>\n"
      "  refcli --host <h> --port <p> admin promote --component <id> --generation <n> --lifecycle <STATE>\n"
      "  refcli --host <h> --port <p> admin drain --component <id> --generation <n>\n"
      "  refcli --host <h> --port <p> admin migrate --plan <id>\n"
      "  refcli --host <h> --port <p> admin rollback --plan <id>\n"
      "  refcli --host <h> --port <p> admin supersede --plan <id>\n"
      "  refcli --host <h> --port <p> admin retire --component <id> --generation <n>\n"
      "  refcli --host <h> --port <p> admin enable-feature --feature <id> --plan <id>\n"
      "  refcli --host <h> --port <p> admin fence --worker <id> --boot <n> [--reason <text>]\n",
      REF_FABRIC_VERSION);
}

ref::CommandResult send_admin(ref::EvolutionClient& client, ref::MessageType type, const ref::WireMessage& payload) {
  ref::Frame response;
  ref::WireMessage body;
  const ref::Status sent = client.request(type, payload, response, body);
  ref::CommandResult result;
  if (sent.is_failure()) {
    result.status = sent;
    result.json = std::string("{\"status\": \"") + ref::to_string(sent.code()) + "\"}";
    return result;
  }
  const auto json = body.document(ref::FieldId::from_valid("json"));
  const auto status = body.text(ref::FieldId::from_valid("status"));
  const auto detail = body.text(ref::FieldId::from_valid("detail"));
  std::fprintf(stderr, "refcli: response type=%s status=%s detail=%s fields=%zu\n",
               ref::to_string(response.header.type), status.has_value() ? std::string(*status).c_str() : "(none)",
               detail.has_value() ? std::string(*detail).c_str() : "(none)", body.size());
  if (status.has_value() && *status == "OK") {
    result.status = ref::Status::ok();
  } else {
    const auto code = status.has_value() ? ref::parse_error_code(*status) : std::nullopt;
    result.status = ref::Status::failure(code.has_value() ? *code : ref::ErrorCode::Internal,
                                         detail.has_value() ? *detail : "request refused");
  }
  result.json = json.has_value() ? std::string(*json) : std::string("{}");
  return result;
}

int run_offline(const std::string& state_path, const std::string& verb, const ref::cli::Arguments& arguments) {
  ref::DurableStore store(state_path);
  if (!store.exists()) {
    std::fprintf(stderr, "refcli: state file not found: %s\n", state_path.c_str());
    return 2;
  }
  ref::DurableState state;
  const ref::Status loaded = store.load(state);
  if (verb == "verify") {
    ref::JsonWriter writer;
    writer.begin_object();
    writer.field("status", loaded.is_ok() ? "OK" : ref::to_string(loaded.code()));
    writer.field("detail", loaded.detail());
    writer.field("path", state_path);
    writer.field("format_version", static_cast<std::uint64_t>(state.format_version));
    writer.field("components", static_cast<std::uint64_t>(state.components.version_count()));
    writer.field("compatibility_edges", static_cast<std::uint64_t>(state.matrix.edge_count()));
    writer.field("plans", static_cast<std::uint64_t>(state.plans.size()));
    writer.field("migrations", static_cast<std::uint64_t>(state.migrations.size()));
    writer.field("retirements", static_cast<std::uint64_t>(state.retirements.size()));
    writer.field("fenced_boots", static_cast<std::uint64_t>(state.fenced_boots.size()));
    writer.end_object();
    std::printf("%s\n", writer.str().c_str());
    return loaded.is_ok() ? 0 : 3;
  }
  if (loaded.is_failure()) {
    std::fprintf(stderr, "refcli: durable state could not be loaded: %s\n", loaded.to_string().c_str());
    return 3;
  }
  const std::string kind = arguments.positional().size() > 1 ? arguments.positional()[1] : "components";
  ref::JsonWriter writer;
  writer.begin_object();
  writer.field("status", "OK");
  writer.field("source", "durable-state-file");
  writer.field("path", state_path);
  writer.field_generation("coordinator_epoch", state.coordinator_epoch);
  writer.field_generation("evolution_epoch", state.evolution_epoch);
  if (kind == "components" || kind == "authority") {
    writer.key("components");
    writer.begin_array();
    for (const auto* version : state.components.all_versions()) {
      writer.begin_object();
      writer.field("component", version->component.view());
      writer.field("version", version->version.to_string());
      writer.field_generation("runtime_generation", version->generation);
      writer.field("lifecycle", ref::to_string(version->lifecycle));
      writer.field("supported_protocols", ref::encode_generation_set(version->protocols.supported));
      writer.field("readable_protocols", ref::encode_generation_set(version->protocols.readable));
      writer.field("writable_protocols", ref::encode_generation_set(version->protocols.writable));
      writer.field("supported_schemas", ref::encode_generation_set(version->schemas.supported));
      writer.field("readable_schemas", ref::encode_generation_set(version->schemas.readable));
      writer.field("writable_schemas", ref::encode_generation_set(version->schemas.writable));
      writer.end_object();
    }
    writer.end_array();
  } else if (kind == "plans") {
    writer.key("plans");
    writer.begin_array();
    for (const auto& [id, plan] : state.plans) {
      (void)id;
      writer.begin_object();
      writer.field("plan", plan.id.view());
      writer.field("component", plan.component.view());
      writer.field_generation("current_generation", plan.current_generation);
      writer.field_generation("candidate_generation", plan.candidate_generation);
      writer.field("stage", ref::to_string(plan.stage));
      writer.field_generation("evolution_epoch", plan.epoch);
      writer.field_generation("protocol_generation", plan.protocol_generation);
      writer.field_generation("schema_generation", plan.schema_generation);
      writer.field("rollback_barrier_crossed", plan.rollback_barrier_crossed);
      writer.field("superseded", plan.superseded);
      writer.end_object();
    }
    writer.end_array();
  } else if (kind == "compatibility") {
    writer.field_generation("matrix_generation", state.matrix.generation());
    writer.key("edges");
    writer.begin_array();
    for (const auto* edge : state.matrix.all_edges()) {
      writer.begin_object();
      writer.field_generation("from", edge->from);
      writer.field_generation("to", edge->to);
      writer.field("permissions", ref::describe_permissions(edge->permissions));
      writer.end_object();
    }
    writer.end_array();
  } else if (kind == "migrations") {
    writer.key("migrations");
    writer.begin_array();
    for (const auto& record : state.migrations) {
      writer.begin_object();
      writer.field("migration", record.id.view());
      writer.field_generation("source", record.source);
      writer.field_generation("target", record.target);
      writer.field("outcome", ref::to_string(record.outcome));
      writer.field("irreversible", record.irreversible);
      writer.end_object();
    }
    writer.end_array();
  } else if (kind == "retirement") {
    writer.key("retirements");
    writer.begin_array();
    for (const auto& [key, record] : state.retirements) {
      (void)key;
      writer.begin_object();
      writer.field("component", record.component.view());
      writer.field_generation("generation", record.generation);
      writer.end_object();
    }
    writer.end_array();
    writer.key("fenced_boots");
    writer.begin_array();
    for (const auto& [key, record] : state.fenced_boots) {
      (void)key;
      writer.begin_object();
      writer.field("worker", record.worker.view());
      writer.field_generation("boot", record.boot);
      writer.field("reason", record.reason.view());
      writer.end_object();
    }
    writer.end_array();
  } else if (kind == "gates") {
    writer.key("gates");
    writer.begin_array();
    for (const auto* gate : state.gates.all()) {
      writer.begin_object();
      writer.field("feature", gate->id.view());
      writer.field("enabled", gate->enabled);
      writer.field_generation("generation", gate->generation);
      writer.end_object();
    }
    writer.end_array();
  } else {
    writer.field("detail", "unsupported offline query kind");
  }
  writer.end_object();
  std::printf("%s\n", writer.str().c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  ref::cli::Arguments arguments(argc, argv);
  if (arguments.has("tool-version")) {
    std::printf("Runtime Evolution Fabric %s\n", REF_FABRIC_VERSION);
    return 0;
  }
  const std::vector<std::string>& positional = arguments.positional();
  if (positional.empty() || arguments.has("help")) {
    print_usage();
    return positional.empty() ? 2 : 0;
  }
  const std::string verb = positional[0];
  const std::string state_path = arguments.get("state");

  if (verb == "show" || verb == "verify" || verb == "explain") {
    if (!state_path.empty() && !arguments.has("host")) {
      return run_offline(state_path, verb == "verify" ? "verify" : "show", arguments);
    }
    ref::ClientConfig config;
    config.host = arguments.get("host", "127.0.0.1");
    config.port = static_cast<std::uint16_t>(arguments.number("port", 0));
    config.operator_mode = true;
    ref::EvolutionClient client(config);
    ref::ProtocolHandshake handshake;
    handshake.component = ref::RuntimeComponentId::from_valid("refcli");
    handshake.version.number = ref::VersionNumber{1, 0, 0};
    handshake.version.artifact.build = ref::BuildId::from_valid("refcli");
    handshake.runtime_generation = ref::RuntimeGeneration::from_raw(1);
    handshake.worker = ref::WorkerId::from_valid("refcli");
    handshake.boot = ref::WorkerBootId::from_raw(1);
    handshake.protocol = ref::ProtocolId::from_valid("ref-wire");
    (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(1));
    (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(2));
    handshake.required_minimum = ref::ProtocolGeneration::from_raw(1);
    (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(1));
    (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(2));
    (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(3));
    handshake.committed_schema = ref::SchemaGeneration::from_raw(1);
    handshake.capability_generation = ref::CapabilityGeneration::from_raw(1);
    handshake.declared_role = ref::NoteText::from_valid("read-only inspector");
    ref::HandshakeOutcome outcome;
    const ref::Status connected = client.connect(handshake, outcome);
    if (connected.is_failure()) {
      std::fprintf(stderr, "refcli: %s\n", connected.to_string().c_str());
      return 3;
    }
    ref::WireMessage payload;
    const std::string query = verb == "explain" ? "explain" : (positional.size() > 1 ? positional[1] : "authority");
    (void)payload.set_text(ref::FieldId::from_valid("query"), query);
    if (arguments.has("component")) {
      (void)payload.set_ident(ref::FieldId::from_valid("component"),
                              ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component")));
    }
    ref::Frame response;
    ref::WireMessage body;
    const ref::Status sent = client.request(ref::MessageType::QueryState, payload, response, body);
    if (sent.is_failure()) {
      std::fprintf(stderr, "refcli: %s\n", sent.to_string().c_str());
      return 3;
    }
    const auto status = body.text(ref::FieldId::from_valid("status"));
    const auto json = body.document(ref::FieldId::from_valid("json"));
    if (status.has_value() && *status != "OK") {
      const auto detail = body.text(ref::FieldId::from_valid("detail"));
      std::fprintf(stderr, "refcli: query refused: %s\n", detail.has_value() ? std::string(*detail).c_str() : "");
      return 4;
    }
    std::printf("%s\n", json.has_value() ? std::string(*json).c_str() : "{}");
    return 0;
  }

  if (verb != "admin") {
    std::fprintf(stderr, "refcli: unknown verb '%s' (see --help)\n", verb.c_str());
    return 2;
  }
  if (positional.size() < 2) {
    std::fprintf(stderr, "refcli: admin requires a command (see --help)\n");
    return 2;
  }
  const std::string command = positional[1];

  ref::ClientConfig config;
  config.host = arguments.get("host", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(arguments.number("port", 0));
  config.operator_mode = true;
  ref::EvolutionClient client(config);
  ref::ProtocolHandshake handshake;
  handshake.component = ref::RuntimeComponentId::from_valid("refcli-operator");
  handshake.version.number = ref::VersionNumber{1, 0, 0};
  handshake.version.artifact.build = ref::BuildId::from_valid("refcli");
  handshake.runtime_generation = ref::RuntimeGeneration::from_raw(1);
  handshake.worker = ref::WorkerId::from_valid("refcli-operator");
  handshake.boot = ref::WorkerBootId::from_raw(1);
  handshake.protocol = ref::ProtocolId::from_valid("ref-wire");
  (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(1));
  (void)handshake.supported_protocols.add(ref::ProtocolGeneration::from_raw(2));
  handshake.required_minimum = ref::ProtocolGeneration::from_raw(1);
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(1));
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(2));
  (void)handshake.supported_schemas.add(ref::SchemaGeneration::from_raw(3));
  handshake.committed_schema = ref::SchemaGeneration::from_raw(1);
  handshake.capability_generation = ref::CapabilityGeneration::from_raw(1);
  handshake.declared_role = ref::NoteText::from_valid("operator");
  ref::HandshakeOutcome outcome;
  const ref::Status connected = client.connect(handshake, outcome);
  if (connected.is_failure()) {
    std::fprintf(stderr, "refcli: %s\n", connected.to_string().c_str());
    return 3;
  }

  ref::MessageType type = ref::MessageType::Invalid;
  ref::WireMessage payload;
  const ref::FieldId f_component = ref::FieldId::from_valid("component");
  const ref::FieldId f_generation = ref::FieldId::from_valid("runtime_generation");
  const ref::FieldId f_plan = ref::FieldId::from_valid("plan");
  if (command == "fence") {
    type = ref::MessageType::Fence;
    (void)payload.set_ident(ref::FieldId::from_valid("worker"),
                            ref::cli::ident_arg<ref::WorkerIdTag, 63>(arguments.get("worker")));
    (void)payload.set_generation(ref::FieldId::from_valid("boot"),
                                 ref::WorkerBootId::from_raw(arguments.number("boot", 0)));
    (void)payload.set_text(ref::FieldId::from_valid("reason"), arguments.get("reason", "operator fence"));
  } else if (command == "advance") {
    type = ref::MessageType::PublishStage;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
    (void)payload.set_text(ref::FieldId::from_valid("stage"), arguments.get("stage"));
    (void)payload.set_generation(ref::FieldId::from_valid("stage_generation"),
                                 ref::StageGeneration::from_raw(arguments.number("stage-generation", 0)));
  } else if (command == "rebind") {
    type = ref::MessageType::RequestPlanRebind;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
    (void)payload.set_generation(ref::FieldId::from_valid("plan_generation"),
                                 ref::EvolutionPlanGeneration::from_raw(arguments.number("plan-generation", 0)));
  } else if (command == "drain" || command == "retire") {
    type = command == "drain" ? ref::MessageType::RequestDrain : ref::MessageType::RequestRetirement;
    (void)payload.set_ident(f_component,
                            ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component")));
    (void)payload.set_generation(f_generation, ref::RuntimeGeneration::from_raw(arguments.number("generation", 0)));
  } else if (command == "publish-component") {
    type = ref::MessageType::RegisterComponent;
    (void)payload.set_ident(f_component,
                            ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component")));
    (void)payload.set_generation(f_generation, ref::RuntimeGeneration::from_raw(arguments.number("generation", 0)));
    (void)payload.set_text(ref::FieldId::from_valid("version"), arguments.get("version", "1.0.0"));
    (void)payload.set_text(ref::FieldId::from_valid("artifact"), arguments.get("artifact", "registered"));
    (void)payload.set_text(ref::FieldId::from_valid("supported_protocols"), arguments.get("protocols", "1"));
    (void)payload.set_text(ref::FieldId::from_valid("readable_protocols"), arguments.get("readable-protocols",
                                                                                        arguments.get("protocols", "1")));
    (void)payload.set_text(ref::FieldId::from_valid("writable_protocols"), arguments.get("writable-protocols",
                                                                                        arguments.get("protocols", "1")));
    (void)payload.set_text(ref::FieldId::from_valid("supported_schemas"), arguments.get("schemas", "1"));
    (void)payload.set_text(ref::FieldId::from_valid("readable_schemas"), arguments.get("readable-schemas",
                                                                                      arguments.get("schemas", "1")));
    (void)payload.set_text(ref::FieldId::from_valid("writable_schemas"), arguments.get("writable-schemas",
                                                                                      arguments.get("schemas", "1")));
    (void)payload.set_generation(ref::FieldId::from_valid("capability_generation"),
                                 ref::CapabilityGeneration::from_raw(1));
    (void)payload.set_text(ref::FieldId::from_valid("lifecycle"), arguments.get("lifecycle", "REGISTERED"));
    (void)payload.set_text(ref::FieldId::from_valid("provenance"), "REAL");
    if (arguments.has("features")) (void)payload.set_text(ref::FieldId::from_valid("features"), arguments.get("features"));
    if (arguments.has("migrations")) {
      (void)payload.set_text(ref::FieldId::from_valid("migrations"), arguments.get("migrations"));
    }
  } else if (command == "publish-compatibility") {
    type = ref::MessageType::PublishCompatibility;
    (void)payload.set_generation(ref::FieldId::from_valid("from_generation"),
                                 ref::RuntimeGeneration::from_raw(arguments.number("from", 0)));
    (void)payload.set_generation(ref::FieldId::from_valid("to_generation"),
                                 ref::RuntimeGeneration::from_raw(arguments.number("to", 0)));
    std::uint64_t permissions = 0;
    for (const std::string& flag : arguments.flags()) {
      static const std::map<std::string, ref::CompatPermission> kPermissions{
          {"control-channel", ref::CompatPermission::ControlChannel},
          {"read-only-messages", ref::CompatPermission::ReadOnlyMessages},
          {"mutating-messages", ref::CompatPermission::MutatingMessages},
          {"read-state", ref::CompatPermission::ReadState},
          {"write-shared-state", ref::CompatPermission::WriteSharedState},
          {"share-snapshots", ref::CompatPermission::ShareSnapshots},
          {"join-control-epoch", ref::CompatPermission::JoinControlEpoch},
          {"coexist-during-rollout", ref::CompatPermission::CoexistDuringRollout},
          {"rollback-after-mutation", ref::CompatPermission::RollbackAfterMutation}};
      const auto it = kPermissions.find(flag);
      if (it != kPermissions.end()) permissions |= static_cast<std::uint64_t>(it->second);
    }
    (void)payload.set_u64(ref::FieldId::from_valid("permissions"), permissions);
    (void)payload.set_text(ref::FieldId::from_valid("provenance"), "REAL");
    // Aspect outcomes are supplied as --aspect NAME=OUTCOME; unspecified aspects
    // default to UNKNOWN so a partially described edge can never look compatible.
    // An operator may publish an explicit profile when every aspect has been
    // assessed; otherwise each aspect must be stated individually. Unstated
    // aspects stay UNKNOWN and therefore never grant permission.
    const std::string profile = arguments.get("profile");
    for (std::size_t i = 0; i < ref::kCompatAspectCount; ++i) {
      const auto aspect = static_cast<ref::CompatAspect>(i);
      std::string name = ref::to_string(aspect);
      std::string lower;
      for (const char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string fallback = "UNKNOWN";
      if (profile == "compatible") {
        fallback = "FULLY_COMPATIBLE";
      } else if (profile == "mixed-version") {
        switch (aspect) {
          case ref::CompatAspect::WireProtocol:
          case ref::CompatAspect::ProtocolRead:
          case ref::CompatAspect::PeerVersion:
          case ref::CompatAspect::Capability:
          case ref::CompatAspect::PersistenceFormat:
          case ref::CompatAspect::Snapshot:
          case ref::CompatAspect::FeatureGate:
          case ref::CompatAspect::StateMigration:
            fallback = "MIXED_VERSION_COMPATIBLE";
            break;
          case ref::CompatAspect::ProtocolWrite:
          case ref::CompatAspect::SchemaWrite:
            fallback = "READ_COMPATIBLE";
            break;
          case ref::CompatAspect::SchemaRead:
            fallback = "COMPATIBLE_AFTER_STATE_MIGRATION";
            break;
          case ref::CompatAspect::Rollback:
            fallback = "ROLLBACK_COMPATIBLE";
            break;
          default:
            fallback = "FULLY_COMPATIBLE";
            break;
        }
      }
      const std::string value = arguments.get("aspect-" + lower, fallback);
      (void)payload.set_text(ref::FieldId::from_valid("aspect_" + lower), value);
    }
  } else if (command == "plan") {
    type = ref::MessageType::RequestUpgrade;
    (void)payload.set_ident(f_component,
                            ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component")));
    (void)payload.set_generation(ref::FieldId::from_valid("candidate_generation"),
                                 ref::RuntimeGeneration::from_raw(arguments.number("candidate", 0)));
    if (arguments.has("plan")) {
      (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
    }
    if (arguments.has("canary")) {
      (void)payload.set_ident(ref::FieldId::from_valid("canary_cohort"),
                              ref::cli::ident_arg<ref::CohortIdTag, 63>(arguments.get("canary")));
    }
    if (arguments.has("cohorts")) {
      (void)payload.set_text(ref::FieldId::from_valid("cohorts"), arguments.get("cohorts"));
    }
    if (arguments.has("migration")) {
      // <id>:<generation>:<source>:<target>
      const std::string spec = arguments.get("migration");
      std::vector<std::string> parts;
      std::size_t start = 0;
      while (start <= spec.size()) {
        const std::size_t colon = spec.find(':', start);
        parts.push_back(spec.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
      }
      if (parts.size() == 4) {
        bool ok = false;
        (void)payload.set_ident(ref::FieldId::from_valid("migration"), ref::MigrationId::from_valid(parts[0]));
        (void)payload.set_generation(ref::FieldId::from_valid("migration_generation"),
                                     ref::MigrationGeneration::from_raw(ref::parse_u64(parts[1], ok)));
        (void)payload.set_generation(ref::FieldId::from_valid("migration_source"),
                                     ref::SchemaGeneration::from_raw(ref::parse_u64(parts[2], ok)));
        (void)payload.set_generation(ref::FieldId::from_valid("migration_target"),
                                     ref::SchemaGeneration::from_raw(ref::parse_u64(parts[3], ok)));
      }
    }
    (void)payload.set_bool(ref::FieldId::from_valid("policy_require_canary"),
                           arguments.number("require-canary", 1) != 0);
    (void)payload.set_bool(ref::FieldId::from_valid("policy_require_mixed_version"),
                           arguments.number("require-mixed-version", 1) != 0);
    (void)payload.set_bool(ref::FieldId::from_valid("policy_require_migration_barrier"),
                           arguments.number("require-migration-barrier", 0) != 0);
    (void)payload.set_bool(ref::FieldId::from_valid("policy_require_new_writer_barrier"),
                           arguments.number("require-new-writer-barrier", 1) != 0);
    (void)payload.set_bool(ref::FieldId::from_valid("policy_require_old_writer_drain"),
                           arguments.number("require-old-writer-drain", 1) != 0);
  } else if (command == "promote") {
    type = ref::MessageType::RequestLifecycle;
    (void)payload.set_ident(f_component,
                            ref::cli::ident_arg<ref::RuntimeComponentIdTag, 63>(arguments.get("component")));
    (void)payload.set_generation(f_generation, ref::RuntimeGeneration::from_raw(arguments.number("generation", 0)));
    (void)payload.set_text(ref::FieldId::from_valid("lifecycle"), arguments.get("lifecycle", "CURRENT"));
  } else if (command == "migrate") {
    type = ref::MessageType::RequestMigrationDispatch;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
  } else if (command == "rollback") {
    type = ref::MessageType::RequestRollbackDispatch;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
  } else if (command == "supersede") {
    type = ref::MessageType::RequestSupersede;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
  } else if (command == "enable-feature") {
    type = ref::MessageType::RequestFeatureEnable;
    (void)payload.set_ident(f_plan, ref::cli::ident_arg<ref::EvolutionPlanIdTag, 63>(arguments.get("plan")));
    (void)payload.set_ident(ref::FieldId::from_valid("feature"),
                            ref::cli::ident_arg<ref::FeatureGateIdTag, 63>(arguments.get("feature")));
  } else {
    std::fprintf(stderr, "refcli: unknown admin command '%s'\n", command.c_str());
    return 2;
  }

  const ref::CommandResult result = send_admin(client, type, payload);
  std::printf("%s\n", result.json.c_str());
  if (result.status.is_failure()) {
    std::fprintf(stderr, "refcli: %s\n", result.status.to_string().c_str());
    return 4;
  }
  return 0;
}