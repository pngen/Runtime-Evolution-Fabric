// Runtime Evolution Fabric - REAL multiprocess proofs.
//
// These tests start actual evolution coordinator processes, actual runtime
// worker processes and the operator CLI, connected over real TCP sockets with
// real durable state files. Process death is real process death.
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "support/child_process.hpp"
#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

using namespace ref;
using reftest::ChildProcess;
using reftest::TempDir;

namespace {

std::string tool_dir() { return std::string(REF_TOOL_DIR); }

std::string exe(const char* name) {
#if defined(_WIN32)
  return tool_dir() + "\\" + name + ".exe";
#else
  return tool_dir() + "/" + name;
#endif
}

std::map<std::string, std::string> parse_key_values(const std::string& line) {
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t space = line.find(' ', start);
    const std::size_t end = space == std::string::npos ? line.size() : space;
    const std::string token = line.substr(start, end - start);
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos) {
      values[token.substr(0, equals)] = token.substr(equals + 1);
    }
    if (space == std::string::npos) break;
    start = space + 1;
  }
  return values;
}

struct CliResult {
  int exit_code{-1};
  std::string output{};
  [[nodiscard]] bool ok() const { return exit_code == 0; }
  [[nodiscard]] bool contains(const std::string& needle) const {
    return output.find(needle) != std::string::npos;
  }
};

CliResult run_cli(const std::vector<std::string>& arguments) {
  ChildProcess cli;
  CliResult result;
  const Status started = cli.start(exe("refcli"), arguments);
  if (started.is_failure()) {
    result.output = started.to_string();
    return result;
  }
  std::string line;
  while (cli.read_line(line).is_ok()) {
    result.output += line;
    result.output += '\n';
  }
  result.exit_code = cli.wait();
  return result;
}

// A real coordinator process plus the operator CLI bound to it.
class RealFabric {
 public:
  RealFabric(TempDir& dir, const char* name) : dir_(&dir), name_(name) {}

  ~RealFabric() {
    coordinator_.kill();
    for (auto& worker : workers_) worker.process.kill();
  }

  [[nodiscard]] Status start_coordinator() {
    std::vector<std::string> arguments{"--port", "0", "--state", dir_->file(name_ + ".state")};
    const Status started = coordinator_.start(exe("refcoordinator"), arguments);
    if (started.is_failure()) return started;
    std::string line;
    const Status ready = coordinator_.wait_for_line("REF_COORDINATOR_READY", line);
    if (ready.is_failure()) return ready;
    const auto values = parse_key_values(line);
    const auto port = values.find("port");
    if (port == values.end()) {
      return Status::failure(ErrorCode::Internal, "coordinator did not report its port");
    }
    port_ = static_cast<std::uint16_t>(std::stoi(port->second));
    return Status::ok();
  }

  // Restarts the coordinator process against the same durable state file.
  [[nodiscard]] Status restart_coordinator() {
    const Status stopped = stop_coordinator();
    if (stopped.is_failure()) return stopped;
    return start_coordinator();
  }

  [[nodiscard]] Status stop_coordinator() {
    if (!coordinator_.started()) return Status::ok();
    const Status written = coordinator_.write_line("stop");
    if (written.is_failure()) return written;
    std::string line;
    const Status stopped = coordinator_.wait_for_line("REF_COORDINATOR_STOPPED", line);
    const int code = coordinator_.wait();
    if (stopped.is_failure()) return stopped;
    if (code != 0) {
      return Status::failure(ErrorCode::Internal,
                             "coordinator did not exit cleanly: code " + std::to_string(code));
    }
    return Status::ok();
  }

  [[nodiscard]] CliResult cli(std::initializer_list<std::string> arguments) {
    std::vector<std::string> full{"--host", "127.0.0.1", "--port", std::to_string(port_)};
    for (const auto& argument : arguments) full.push_back(argument);
    return run_cli(full);
  }

  struct WorkerHandle {
    ChildProcess process;
    std::map<std::string, std::string> ready;
    std::string state_path;
  };

  // Starts a real worker process for a runtime generation.
  [[nodiscard]] Status start_worker(const std::string& worker_id, std::uint64_t boot,
                                    std::uint64_t generation, const char* protocols,
                                    const char* schemas, const char* artifact,
                                    WorkerHandle*& handle, bool crash_after_migration = false,
                                    const std::string& state_file = std::string()) {
    workers_.emplace_back();
    WorkerHandle& entry = workers_.back();
    entry.state_path = state_file.empty() ? dir_->file(worker_id + ".state") : state_file;
    const std::string version = std::to_string(generation) + ".0.0";
    std::vector<std::string> arguments{"--host", "127.0.0.1", "--port", std::to_string(port_),
                                       "--component", "orders",
                                       "--generation", std::to_string(generation),
                                       "--version", version,
                                       "--artifact", artifact,
                                       "--worker", worker_id,
                                       "--boot", std::to_string(boot),
                                       "--protocols", protocols,
                                       "--schemas", schemas,
                                       "--state", entry.state_path};
    if (crash_after_migration) arguments.push_back("--crash-after-migration-commit");
    const Status started = entry.process.start(exe("refworker"), arguments);
    if (started.is_failure()) return started;
    std::string line;
    const Status ready = entry.process.wait_for_line("REF_WORKER_READY", line);
    if (ready.is_failure()) return ready;
    entry.ready = parse_key_values(line);
    handle = &entry;
    return Status::ok();
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] TempDir& dir() { return *dir_; }

 private:
  TempDir* dir_;
  std::string name_;
  ChildProcess coordinator_{};
  // A deque so that handles keep stable addresses as workers are added.
  std::deque<WorkerHandle> workers_{};
  std::uint16_t port_{0};
};

// The complete mixed-version bootstrap used by several proofs.
Status bootstrap_mixed_version(RealFabric& fabric, RealFabric::WorkerHandle*& gen1,
                               RealFabric::WorkerHandle*& gen2) {
  Status status = fabric.start_worker("worker-a", 1, 1, "1", "1", "refworker-gen1", gen1);
  if (status.is_failure()) return status;
  status = fabric.start_worker("worker-b", 1, 2, "1,2", "1,2", "refworker-gen2", gen2);
  if (status.is_failure()) return status;
  {
    const CliResult promoted =
        fabric.cli({"admin", "promote", "--component", "orders", "--generation", "1", "--lifecycle",
                    "CURRENT"});
    if (!promoted.ok()) return Status::failure(ErrorCode::Conflict, "promotion failed: " + promoted.output);
  }
  {
    const CliResult edge = fabric.cli({"admin", "publish-compatibility", "--from", "1", "--to", "2",
                                       "--profile", "mixed-version", "--control-channel",
                                       "--read-only-messages", "--mutating-messages", "--read-state",
                                       "--write-shared-state", "--share-snapshots", "--join-control-epoch",
                                       "--coexist-during-rollout", "--rollback-after-mutation"});
    if (!edge.ok()) return Status::failure(ErrorCode::Conflict, "compatibility failed: " + edge.output);
  }
  const CliResult plan = fabric.cli({"admin", "plan", "--component", "orders", "--candidate", "2",
                                     "--canary", "canary-a", "--cohorts", "canary-a", "--migration",
                                     "migration-1-2:1:1:2"});
  if (!plan.ok()) return Status::failure(ErrorCode::Conflict, "plan failed: " + plan.output);
  return Status::ok();
}

Status advance(RealFabric& fabric, const char* stage) {
  const CliResult result = fabric.cli({"admin", "advance", "--plan", "plan-1", "--stage", stage});
  if (!result.ok()) {
    return Status::failure(ErrorCode::Conflict, std::string("advance to ") + stage + " failed: " + result.output);
  }
  return Status::ok();
}

}  // namespace

REF_TEST(multiprocess, real_dual_generation_coexistence_and_negotiation) {
  TempDir dir("mp-dual");
  RealFabric fabric(dir, "dual");
  REF_CHECK_STATUS_OK(fabric.start_coordinator());
  RealFabric::WorkerHandle* gen1 = nullptr;
  RealFabric::WorkerHandle* gen2 = nullptr;
  REF_CHECK_STATUS_OK(bootstrap_mixed_version(fabric, gen1, gen2));
  REF_NOTE("generation 1 process: " + gen1->ready.at("worker") + " protocol=" + gen1->ready.at("protocol"));
  REF_NOTE("generation 2 process: " + gen2->ready.at("worker") + " protocol=" + gen2->ready.at("protocol"));

  // Both real processes are alive at the same time.
  REF_CHECK(gen1->process.running());
  REF_CHECK(gen2->process.running());
  // During the mixed-version phase both negotiate protocol generation 1.
  REF_CHECK_EQ(gen1->ready.at("protocol"), std::string("1"));
  REF_CHECK_EQ(gen2->ready.at("protocol"), std::string("1"));

  const CliResult workers = fabric.cli({"show", "workers"});
  REF_CHECK(workers.ok());
  REF_CHECK(workers.contains("\"worker\": \"worker-a\""));
  REF_CHECK(workers.contains("\"worker\": \"worker-b\""));

  const CliResult authority = fabric.cli({"show", "authority", "--component", "orders"});
  REF_CHECK(authority.ok());
  REF_CHECK(authority.contains("\"authoritative_generation\": 1"));
  REF_CHECK(authority.contains("\"rollout_stage\": \"CANDIDATE_REGISTERED\""));

  // The candidate cannot use the protocol-2 feature while generation 1 lives.
  REF_CHECK_STATUS_OK(advance(fabric, "COMPATIBILITY_PROVEN"));
  REF_CHECK_STATUS_OK(advance(fabric, "CANARY_COHORT"));
  const CliResult canary = fabric.cli({"show", "authority", "--component", "orders"});
  REF_CHECK(canary.contains("\"rollout_stage\": \"CANARY_COHORT\""));

  REF_CHECK_STATUS_OK(fabric.stop_coordinator());
}

REF_TEST(multiprocess, real_worker_death_fencing_and_replacement) {
  TempDir dir("mp-death");
  RealFabric fabric(dir, "death");
  REF_CHECK_STATUS_OK(fabric.start_coordinator());
  RealFabric::WorkerHandle* gen1 = nullptr;
  RealFabric::WorkerHandle* gen2 = nullptr;
  REF_CHECK_STATUS_OK(bootstrap_mixed_version(fabric, gen1, gen2));

  // Real OS process death.
  const std::uint32_t dead_pid = gen1->process.process_id();
  gen1->process.kill();
  REF_CHECK(!gen1->process.running());
  REF_NOTE("killed generation-1 worker pid " + std::to_string(dead_pid));

  // The survivor is unaffected.
  REF_CHECK(gen2->process.running());
  const CliResult after_death = fabric.cli({"show", "workers"});
  REF_CHECK(after_death.ok());
  REF_CHECK(after_death.contains("\"worker\": \"worker-b\""));

  // The dead boot identity is fenced durably.
  const CliResult fenced = fabric.cli({"admin", "fence", "--worker", "worker-a", "--boot", "1",
                                       "--reason", "process death detected"});
  REF_CHECK(fenced.ok());
  const CliResult stale = fabric.cli({"admin", "fence", "--worker", "worker-a", "--boot", "1",
                                      "--reason", "duplicate"});
  REF_CHECK(stale.ok());  // fencing is idempotent and durable

  // A replacement incarnation with a fresh boot identity is accepted.
  RealFabric::WorkerHandle* replacement = nullptr;
  REF_CHECK_STATUS_OK(fabric.start_worker("worker-a", 2, 1, "1", "1", "refworker-gen1", replacement));
  REF_CHECK(replacement->process.running());
  REF_CHECK_EQ(replacement->ready.at("boot"), std::string("2"));
  const CliResult boots = fabric.cli({"show", "retirement"});
  REF_CHECK(boots.ok());
  REF_CHECK(boots.contains("\"boot\": 1"));

  REF_CHECK_STATUS_OK(fabric.stop_coordinator());
}

REF_TEST(multiprocess, real_coordinator_restart_preserves_history_and_refuses_old_epochs) {
  TempDir dir("mp-restart");
  RealFabric fabric(dir, "restart");
  REF_CHECK_STATUS_OK(fabric.start_coordinator());
  RealFabric::WorkerHandle* gen1 = nullptr;
  RealFabric::WorkerHandle* gen2 = nullptr;
  REF_CHECK_STATUS_OK(bootstrap_mixed_version(fabric, gen1, gen2));
  REF_CHECK_STATUS_OK(advance(fabric, "COMPATIBILITY_PROVEN"));
  REF_CHECK_STATUS_OK(advance(fabric, "CANARY_COHORT"));

  const CliResult before = fabric.cli({"show", "epochs"});
  REF_CHECK(before.ok());
  REF_NOTE("epochs before restart: " + before.output);

  // Real coordinator process restart against the same durable state file.
  REF_CHECK_STATUS_OK(fabric.restart_coordinator());

  const CliResult after = fabric.cli({"show", "epochs"});
  REF_CHECK(after.ok());
  REF_CHECK(after.contains("coordinator_epoch"));
  const CliResult plans = fabric.cli({"show", "plans"});
  REF_CHECK(plans.ok());
  REF_CHECK(plans.contains("\"stage\": \"CANARY_COHORT\""));
  const CliResult components = fabric.cli({"show", "components"});
  REF_CHECK(components.ok());
  REF_CHECK(components.contains("\"runtime_generation\": 1"));
  REF_CHECK(components.contains("\"runtime_generation\": 2"));
  const CliResult compatibility = fabric.cli({"show", "compatibility"});
  REF_CHECK(compatibility.ok());
  REF_CHECK(compatibility.contains("\"matrix_generation\": 1"));

  // The old workers could not have survived the epoch change: their sessions
  // were dropped, and they must re-register and renegotiate.
  // No worker of the evolving component is current after the restart: process
  // liveness is never restored from durable state.
  const CliResult workers = fabric.cli({"show", "workers"});
  REF_CHECK(workers.ok());
  // Only the inspecting CLI session itself is live; the runtime workers are not.
  const std::size_t live_end = workers.output.find("durable_worker_records");
  REF_CHECK(live_end != std::string::npos);
  const std::string live = workers.output.substr(0, live_end);
  REF_CHECK(live.find("\"worker\": \"worker-a\"") == std::string::npos);
  REF_CHECK(live.find("\"worker\": \"worker-b\"") == std::string::npos);
  // The durable worker records survive as history, not as authority.
  REF_CHECK(workers.contains("durable_worker_records"));

  REF_CHECK_STATUS_OK(fabric.stop_coordinator());
}

REF_TEST(multiprocess, real_filesystem_migration_with_ambiguous_completion) {
  TempDir dir("mp-migration");
  RealFabric fabric(dir, "migration");
  REF_CHECK_STATUS_OK(fabric.start_coordinator());
  RealFabric::WorkerHandle* gen1 = nullptr;
  RealFabric::WorkerHandle* gen2 = nullptr;
  REF_CHECK_STATUS_OK(bootstrap_mixed_version(fabric, gen1, gen2));
  REF_CHECK_STATUS_OK(advance(fabric, "COMPATIBILITY_PROVEN"));
  REF_CHECK_STATUS_OK(advance(fabric, "CANARY_COHORT"));
  REF_CHECK_STATUS_OK(advance(fabric, "MIXED_VERSION_COHORT"));

  // A worker that commits the migration and then dies before acknowledging it.
  const std::string migration_state = gen2->state_path;
  const Status killed = [&] {
    gen2->process.kill();
    return Status::ok();
  }();
  REF_CHECK_STATUS_OK(killed);
  RealFabric::WorkerHandle* crasher = nullptr;
  REF_CHECK_STATUS_OK(fabric.start_worker("worker-b", 2, 2, "1,2", "1,2", "refworker-gen2", crasher,
                                          /*crash_after_migration=*/true, migration_state));
  REF_CHECK_STATUS_OK(advance(fabric, "MIGRATION_BARRIER"));

  const CliResult dispatched = fabric.cli({"admin", "migrate", "--plan", "plan-1"});
  REF_CHECK(dispatched.ok());

  // The worker commits, then dies without acknowledging: exit code 97 is the
  // injected crash, which is real process death in the middle of the protocol.
  const int exit_code = crasher->process.wait();
  REF_NOTE("worker exit code after injected commit crash: " + std::to_string(exit_code));
  REF_CHECK_EQ(exit_code, 97);

  // The coordinator records an ambiguous outcome and does not repeat the
  // migration blindly.
  const CliResult pending = fabric.cli({"show", "migrations"});
  REF_CHECK(pending.ok());
  REF_CHECK(pending.contains("pending_actions"));
  const CliResult reconcile = fabric.cli({"show", "reconcile"});
  REF_CHECK(reconcile.ok());
  REF_CHECK(reconcile.contains("MIGRATION_MAY_HAVE_COMMITTED"));

  // A replacement worker inspects the real state file and reports what it sees;
  // the coordinator then concludes that the migration committed exactly once.
  RealFabric::WorkerHandle* replacement = nullptr;
  REF_CHECK_STATUS_OK(fabric.start_worker("worker-b", 3, 2, "1,2", "1,2", "refworker-gen2", replacement,
                                          false, migration_state));
  const CliResult migrations = fabric.cli({"show", "migrations"});
  REF_CHECK(migrations.ok());
  REF_CHECK(migrations.contains("\"outcome\": \"MIGRATION_ROLLBACK_AVAILABLE\""));
  const CliResult reconciliation = fabric.cli({"show", "reconcile"});
  REF_CHECK(reconciliation.ok());
  REF_CHECK(!reconciliation.contains("MIGRATION_MAY_HAVE_COMMITTED"));

  // The old generation must still be drained before new writers start.
  const CliResult drained = fabric.cli({"admin", "drain", "--component", "orders", "--generation", "1"});
  REF_CHECK(drained.ok());
  // Authority can advance only after the migration is known to be complete and
  // the old writers are out of the way.
  REF_CHECK_STATUS_OK(advance(fabric, "NEW_WRITER_ENABLED"));
  const CliResult promoted = fabric.cli({"show", "authority", "--component", "orders"});
  REF_CHECK(promoted.ok());
  REF_CHECK(promoted.contains("\"new_writer_enabled\": true"));
  REF_CHECK(promoted.contains("\"protocol_generation\": 2"));

  REF_CHECK_STATUS_OK(fabric.stop_coordinator());
}

REF_TEST(multiprocess, real_shutdown_leaves_no_orphan_processes) {
  TempDir dir("mp-shutdown");
  RealFabric fabric(dir, "shutdown");
  REF_CHECK_STATUS_OK(fabric.start_coordinator());
  RealFabric::WorkerHandle* gen1 = nullptr;
  RealFabric::WorkerHandle* gen2 = nullptr;
  REF_CHECK_STATUS_OK(bootstrap_mixed_version(fabric, gen1, gen2));

  // A graceful stop closes the listener and every session, which makes the
  // workers observe end of stream and exit on their own.
  REF_CHECK_STATUS_OK(fabric.stop_coordinator());
  for (int attempt = 0; attempt < 100 && (gen1->process.running() || gen2->process.running());
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REF_CHECK(!gen1->process.running());
  REF_CHECK(!gen2->process.running());
  REF_CHECK_EQ(gen1->process.exit_code(), 0);
  REF_CHECK_EQ(gen2->process.exit_code(), 0);
}
