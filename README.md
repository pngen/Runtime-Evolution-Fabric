# Runtime Evolution Fabric

A vendor-neutral **C++20 runtime for governing live evolution of distributed runtime components**:
version compatibility, staged rollout, mixed-version coexistence, schema and protocol migration,
state transformation, rollback, drain, restart boundaries and generation-bound authority — without
requiring a global infrastructure shutdown.

Runtime Evolution Fabric 1.0.0.

## The systems question

> How may a live distributed runtime move from generation N to generation N+1 without globally
> stopping service, while preserving compatibility, authority, state integrity, protocol
> correctness, rollback safety, and deterministic control over which versions may communicate,
> mutate state, and become authoritative?

The fabric exists to keep these two pairs apart:

| Weaker statement | What the fabric actually requires |
| --- | --- |
| A newer binary is running. | This exact runtime generation has passed compatibility gates, entered the authorized rollout stage, migrated or reconciled required state, joined the permitted mixed-version set, and may now exercise authority under the current evolution epoch. |
| Old and new processes are alive simultaneously. | These exact runtime generations are explicitly allowed to coexist and exchange only the protocol and state forms proven compatible under the current evolution plan. |

No answer to that question is produced by two versions being able to connect to each other. The
authority view, the compatibility matrix, the rollback barrier, the migration records and the
retirement table are the answer, and all of them are generation-bound.

## Exact boundary

**Owned by this fabric**

Runtime component identity; runtime version identity; runtime generation; protocol generation;
state-schema generation; compatibility generation; evolution-plan identity and generation; evolution
epoch; rollout stage; version cohort; mixed-version coexistence rules; compatibility negotiation;
upgrade eligibility; upgrade authority; drain authority; restart sequencing; state migration
authority; state transformation tracking; protocol migration; message compatibility; feature-gate
generation; capability generation; downgrade and rollback eligibility; rollback authority;
supersession; retirement of old runtime generations; stale-version fencing; stale-protocol and
stale-schema rejection; generation-bound communication authority; durable evolution state;
conservative recovery; distributed coordinator and worker authority; deterministic explanations;
immutable snapshots; REAL / SYNTHETIC / UNSUPPORTED provenance.

**Not owned by this fabric** (integration only)

Generic application deployment; container orchestration; Kubernetes; binary distribution; package
manager behaviour; source compilation; arbitrary firmware upgrades; generic CI/CD; workload
scheduling; model lifecycle; replica scheduling; resource allocation; network routing; cluster
membership; business rollout policy unrelated to runtime compatibility; arbitrary database
migration; generic service discovery.

**Adjacent runtimes.** A *Runtime Registry* may describe available components; a *Compatibility
Registry* may own canonical compatibility facts; *Artifact Fabric* may hold binaries; *Model
Lifecycle Fabric* governs model-version evolution; *Cluster Fabric* governs cluster composition.
This fabric consumes those facts and decides whether runtime generations may coexist, communicate,
migrate state, advance authority, roll back and retire. It does not absorb them.

## Architecture

    include/ref/          public headers (ids, support, component, compat, protocol, schema,
                          migration, plan, authority, store, wire, coordinator, client, worker)
    src/                  implementation, one translation unit per subsystem
    apps/                 refcoordinator, refworker, refcli
    examples/             nine focused examples
    tests/                unit, adversarial, property, race and real multiprocess proofs
    benchmarks/           completed-operation benchmarks across fleet sizes

Process model: one `EvolutionCoordinator` (acceptor thread, one reader thread and one writer thread
per connection, and exactly one event-processing thread that owns all state mutation), any number of
`RuntimeWorker` processes, and the read-only `refcli` inspection surface with a separate
`refcli admin` mutation surface. Workers speak bounded, versioned, integrity-checked framing over
real TCP sockets.

Global lock order, with a debug-build reentrancy detector that aborts on violation:

1. `state_mutex_` — durable plus dynamic evolution state; never held across socket I/O, file I/O,
   callbacks, joins or process waits.
2. `pending_mutex_` — outbound action ledger.
3. `connections_mutex_` — session table.
4. per-session outbound queue — leaf.

A session object is destroyed only by the reaper on the accept or event thread, never on one of its
own threads, because destroying a session on its own reader thread would destroy a joinable
`std::thread` and terminate the process.

## Runtime and version identity

    RuntimeComponentId            stable component identity
    RuntimeVersionId              structured version number + artifact identity
    RuntimeGeneration             the runtime behaviour identity that authority is bound to
    ProtocolGeneration            wire contract identity
    SchemaGeneration              state schema identity
    StateFormatGeneration         durable container format identity
    CompatibilityGeneration       compatibility evidence revision
    EvolutionPlanId               plan identity
    EvolutionPlanGeneration       plan revision; a rebind invalidates bound callers
    EvolutionEpoch                authority epoch of an evolution
    CoordinatorEpoch              coordinator incarnation
    RolloutStageId                stage instance identity
    StageGeneration               stage advance counter
    CohortId                      rollout scope identity
    FeatureGateId                 feature identity
    FeatureGateGeneration         feature-gate table revision
    CapabilityGeneration          advertised capability set revision
    MigrationId                   migration identity
    MigrationGeneration           migration execution attempt
    RollbackId                    rollback identity
    RollbackGeneration            rollback attempt revision
    WorkerId                      process identity
    WorkerBootId                  process incarnation
    EvidenceSourceId              evidence provenance source
    EvidenceGeneration            evidence freshness counter
    SnapshotGeneration            immutable snapshot revision
    PolicyGeneration              evolution policy revision

Semantic-version strings alone are never an identity: a `RuntimeVersionId` carries a structured
version number plus an artifact identity (build label and content digest), and a runtime generation is
bound to exactly one artifact — re-registering the same generation with a different artifact is a
conflict, not an update.

## Lifecycle

`REGISTERED → COMPATIBILITY_PENDING → ELIGIBLE → UPGRADE_PENDING → CANARY_ACTIVE →
MIXED_VERSION_ACTIVE → ROLLOUT_ACTIVE → CURRENT → DRAINING → RETIREMENT_PENDING → RETIRED`, plus
`ROLLBACK_PENDING → ROLLING_BACK → ROLLED_BACK`, `REVALIDATION_REQUIRED` and `FAILED`. Every
transition is checked against an explicit precondition table; `RETIRED` has no outgoing transition
and is terminal for that runtime generation. Eligibility is *derived from evidence*: publishing a
compatibility edge that permits a control channel and coexistence moves the candidate to
`ELIGIBLE`. An operator can move a lifecycle explicitly, but never past a missing precondition.

## Compatibility model

Fourteen aspects are assessed separately for every ordered generation pair:

    BinaryApi, Abi, WireProtocol, ProtocolRead, ProtocolWrite, SchemaRead, SchemaWrite,
    StateMigration, PeerVersion, FeatureGate, PersistenceFormat, Snapshot, Rollback, Capability

with structured outcomes: `FULLY_COMPATIBLE`, `READ_COMPATIBLE`, `WRITE_COMPATIBLE`,
`MIXED_VERSION_COMPATIBLE`, `COMPATIBLE_WITH_FEATURE_GATE`,
`COMPATIBLE_AFTER_STATE_MIGRATION`, `COMPATIBLE_AFTER_PROTOCOL_NEGOTIATION`,
`ROLLBACK_COMPATIBLE`, `INCOMPATIBLE_PROTOCOL`, `INCOMPATIBLE_SCHEMA`, `INCOMPATIBLE_ABI`,
`INCOMPATIBLE_FEATURE_SET`, `INCOMPATIBLE_ROLLBACK`, `INCOMPATIBLE_PEER`, `UNKNOWN`,
`STALE_EVIDENCE` and `UNSUPPORTED`.

Permissions are *derived* from the aspects by one deterministic function and are re-validated when
durable state is loaded: an edge whose stored permissions disagree with its aspects is corrupt state
and is rejected. `UNKNOWN` and `STALE_EVIDENCE` are blocking; nothing becomes compatible by
assumption. The matrix is ordered by (from, to) with an ordered adjacency index: lookup is
logarithmic, the neighbour list is sorted, and coexistence decisions are therefore deterministic.

## Protocol evolution and negotiation

Protocol generations declare message types, per-field introduction generations, required fields,
unknown-field and unknown-message policy, and a minimum safety generation. The handshake exchanges
peer runtime generation, boot identity, supported protocol generations, required minimum, feature
support, epochs, capability generation and committed schema. Negotiation selects the highest common
generation that satisfies the required messages and never drops below the safety floor: a peer that
cannot satisfy the required semantics is rejected instead of silently downgraded.

A frame that parses is not a licence to mutate. Negotiation returns an operation class
(`NONE`, `READ_ONLY`, `RESTRICTED_MUTATION`, `FULL_MUTATION`) and every mutating message is
checked against it. Protocol generation 2 introduces `REQUEST_STATE_TRANSFORM`; during
mixed-version operation the plan pins protocol generation 1, so that message is unavailable until the
old generation is drained and the peers renegotiate.

## Schema evolution

Schema generations declare fields (type, introduced generation, removal, required, irreversible,
required default), readable formats, writable formats, reverse-migration target, canonicalisation rule
and integrity semantics. Read and write compatibility are separate questions with separate answers: a
v2 runtime reads v1 state but v1 cannot read v2 state, and a v2 writer may still emit the v1 form
while a v3 writer may not. Mixed-version operation therefore distinguishes old-format write mode,
dual-write capability, restricted mutation, feature gating, read-only old peers and the migration
barrier before new writes.

## Mixed-version operation

Coexistence is explicit. For every ordered pair the matrix records whether the generations may
establish a control channel, exchange read-only messages, exchange mutating messages, read each
other's persisted state, write state consumable by the other, share snapshots, join one control
epoch, coexist during rollout, and roll back after state mutation — plus whether a feature gate, a
protocol downgrade or a state translation is required. Connection success grants nothing on its own.

## Evolution plans, rollout stages and cohorts

A plan binds current and candidate generation, compatibility-matrix generation, protocol generation,
schema generation, feature-gate generation, migration, rollback target, rollback barrier, required
evidence, policy, cohorts, evolution epoch and coordinator epoch. If any bound generation moves, the
plan is stale and refuses to advance until it is explicitly rebound; stage advancement also requires
the stage generation the caller last saw, so a late or duplicated advance is rejected.

Stages: `CANDIDATE_REGISTERED`, `COMPATIBILITY_PROVEN`, `CANARY_COHORT`,
`MIXED_VERSION_COHORT`, `EXPANDED_COHORT`, `MIGRATION_BARRIER`, `NEW_WRITER_ENABLED`,
`OLD_WRITER_DRAIN`, `FULL_PROMOTION` and `OLD_GENERATION_RETIREMENT`, with `SUPERSEDED` and
`ROLLED_BACK` as side states. Stages that policy marks optional may be skipped, but their lifecycle
effect is still applied: the candidate is walked through every intermediate state. A canary never
receives full mutation authority — during `CANARY_COHORT` the candidate's operation class is
`READ_ONLY`, during `MIXED_VERSION_COHORT` it is `RESTRICTED_MUTATION`, and only
`NEW_WRITER_ENABLED` and later grant `FULL_MUTATION`.

## Feature gates

A gate binds a feature id, minimum runtime, protocol and schema generation, a required peer outcome,
a scope and its own gate generation. Enabling a gate requires the candidate to declare support and
requires the plan's protocol and schema generations to satisfy the gate; enabling bumps the gate
generation and rebinds the active plans explicitly. A feature never becomes available because one new
runtime process exists.

## New-writer activation and drain

`NEW_WRITER_ENABLED` is a distinct authority transition. It requires the migration to be complete,
old incompatible writers drained or fenced, and a valid rollback path or a recorded barrier. Once
crossed, the plan's protocol generation moves to the candidate's highest supported generation, its
schema generation moves to the candidate's native format, and the coordinator re-derives authority
for every live session of the component so the change takes effect on the next frame. Drain marks
workers as draining, sends them a drain request and moves the generation to `DRAINING`; retirement
is blocked until processes have exited or been fenced, pending work has cleared, migration is
complete, the rollback policy permits it and required checkpoints are retained.

## State migration

Migration follows *inspect → verify source generation → prepare → stage temporary state → validate →
commit the generation transition → preserve rollback metadata*. The authoritative file is never
mutated in place: a staged file is written, re-read, verified and atomically renamed over the target,
with a checkpoint written first when rollback metadata is preserved. A migration binds source and
target schema generation, runtime generation, migration generation, evolution epoch, policy
generation and plan.

Outcomes: `MIGRATION_NOT_REQUIRED`, `MIGRATION_READY`, `MIGRATION_COMMITTED`,
`MIGRATION_BLOCKED`, `MIGRATION_FAILED`, `MIGRATION_ROLLBACK_AVAILABLE`,
`MIGRATION_IRREVERSIBLE`, `REVALIDATION_REQUIRED` and `OUTCOME_UNKNOWN`.

A migration is irreversible exactly when no transform exists back from the target generation. An
irreversible migration is refused unless it is explicitly acknowledged, and the transform is real:
the v2 → v3 transform replaces a plaintext token with its digest, which cannot be reconstructed.

## Irreversible barriers, rollback and supersession

Crossing the barrier sets `rollback_barrier_crossed` durably. A rollback request validates the
current stage, current and target generation, target lifecycle (a `RETIRED` target never regains
authority), reverse-migration availability, schema reversibility, reverse control-channel
compatibility, target process availability, evidence currentness and policy.

Outcomes: `ROLLBACK_ALLOWED`, `ROLLBACK_REQUIRES_STATE_RESTORE`,
`ROLLBACK_REQUIRES_PROTOCOL_DOWNGRADE`, `ROLLBACK_BLOCKED_IRREVERSIBLE_MIGRATION`,
`ROLLBACK_BLOCKED_SCHEMA`, `ROLLBACK_BLOCKED_PROTOCOL`, `ROLLBACK_BLOCKED_RETIRED_TARGET`,
`ROLLBACK_REVALIDATION_REQUIRED`, `ROLLBACK_COMMITTED` and `ROLLBACK_FAILED`.

A superseded plan cannot advance stages, enable features, dispatch migration or accept late
completions; its history is preserved as historical record.

## Persistence and recovery

Durable state holds runtime and component identities, versions and generations, the compatibility
matrix, protocol and schema descriptors, plans and committed stage history, migration records,
rollback records, rollback barriers, feature gates, retirement records, fenced boots, worker records
and replay watermarks. It is a versioned, magic-prefixed, length-checked, CRC- and digest-protected,
canonically encoded image written atomically (write to a temporary sibling, then atomic replacement).
A failed load never partially applies: decoding builds a temporary structure and only hands it over
after every cross-section consistency check passes.

What is **not** persisted: process liveness, worker boot authority, dynamic health and connection
state. After a restart the coordinator epoch advances, every live worker is marked non-current, old
sessions are gone, the durable structure and history are restored exactly, irreversible barriers are
preserved, and workers must re-register and renegotiate. Old-epoch frames are rejected.

## Reconciliation

Reconciliation compares durable expectations against live reality and reports structured findings —
candidate expected alive but absent, old runtime still alive after drain, migration may have
committed, feature gate differs from expectation, schema generation advanced externally, protocol
generation differs, candidate started outside the coordinator, retired runtime reconnected, stale
evidence, unexpected lifecycle — each with a conservative-action flag. It never rewrites durable
history. An ambiguous migration outcome (the worker committed and died before acknowledging) is
resolved from observed state facts exactly once: the worker reports the schema generation it actually
sees, and the coordinator draws the conclusion.

## Distributed authority

A process binds runtime generation, worker identity, boot identity, coordinator epoch, evolution
epoch, protocol generation, schema generation, capability generation and evidence generation. A
worker needs a strictly newer boot identity to replace a previous incarnation; a fenced boot is
fenced durably and never reconnects; a retired generation cannot re-register. Every frame is validated
for magic, transport version, message type, flags, length, integrity, runtime generation, protocol
generation, schema generation, coordinator epoch, evolution epoch, boot identity and sequence
watermark before any handler runs.

## REAL / SYNTHETIC / UNSUPPORTED

* **REAL** — independent coordinator and worker processes, real TCP sockets, real process death and
  reincarnation, real coordinator restart, real filesystem state migration, real artifact identities
  and the installed C++ toolchain metadata. The five multiprocess proofs in
  `tests/test_multiprocess.cpp` are REAL.
* **SYNTHETIC** — larger fleets, hundreds of workers, cross-site evolution and hardware firmware
  evolution are not exercised here. The property tests build synthetic fleets through the production
  interfaces and are labelled as such.
* **UNSUPPORTED** — production cluster orchestration, remote fleet rollout and third-party runtime
  migration of binaries this build does not control. Nothing in this repository claims them.

## Build

Requirements: CMake 3.20+, a C++20 compiler (MSVC 19.4x, GCC 11+, Clang 14+) and a TCP stack.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Options: `REF_BUILD_TESTS`, `REF_BUILD_EXAMPLES`, `REF_BUILD_BENCHMARKS`, `REF_BUILD_TOOLS`,
`REF_WARNINGS_AS_ERRORS` (default ON) and `REF_ENABLE_ASAN` (default OFF).

First-party code builds with `/W4 /WX /permissive- /utf-8` on MSVC and with
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
-Wcast-qual -Wdouble-promotion -Werror` elsewhere. There are zero first-party warnings.

## Tests

    ctest --test-dir build --output-on-failure
    ./build/tests/ref_tests                 # every suite
    ./build/tests/ref_tests --filter race   # one suite
    ./build/tests/ref_tests --list

The suite has 64 tests and roughly 1,400 checks: identity, codec, compatibility, protocol and schema
units; real-filesystem migration and irreversible-barrier proofs; persistence and protocol
adversarial suites; coordinator behaviour over real sockets; seeded randomized property tests;
deterministic interleaving tests; and five real multiprocess proofs (dual-generation coexistence,
worker death with fencing and replacement, coordinator restart, ambiguous migration completion, and
clean shutdown without orphan processes). No test uses a timeout; a hanging test is treated as a
defect rather than something to wait out.

## Examples

    ./build/examples/ref_example_01_basic_upgrade
    ./build/examples/ref_example_02_mixed_version_protocol
    ./build/examples/ref_example_03_schema_migration
    ./build/examples/ref_example_04_feature_gate
    ./build/examples/ref_example_05_rollback
    ./build/examples/ref_example_06_irreversible_barrier
    ./build/examples/ref_example_07_worker_fencing
    ./build/examples/ref_example_08_coordinator_restart
    ./build/examples/ref_example_09_installed_consumer

## CLI

Read-only inspection:

    refcli --state <file> verify
    refcli --state <file> show <components|plans|compatibility|migrations|gates|retirement>
    refcli --host 127.0.0.1 --port <port> show <kind> [--component <id>]
    refcli --host 127.0.0.1 --port <port> explain [--component <id>]

`show` accepts: components, plans, compatibility, protocols, schemas, gates, migrations, workers,
epochs, retirement, snapshots, reconcile and authority. Administrative mutation is separate:

    refcli --host <h> --port <p> admin publish-component --component <id> --generation <n>
          --version <version> --protocols <csv> --schemas <csv> [--lifecycle <STATE>] [--artifact <id>]
    refcli --host <h> --port <p> admin promote --component <id> --generation <n> --lifecycle <STATE>
    refcli --host <h> --port <p> admin publish-compatibility --from <n> --to <n>
          [--profile compatible|mixed-version] [--aspect-<aspect> <OUTCOME>] [--control-channel ...]
    refcli --host <h> --port <p> admin plan --component <id> --candidate <n> [--canary <cohort>]
          [--cohorts <csv>] [--migration <id>:<generation>:<source>:<target>]
    refcli --host <h> --port <p> admin advance --plan <id> --stage <STAGE> [--stage-generation <n>]
    refcli --host <h> --port <p> admin rebind --plan <id>
    refcli --host <h> --port <p> admin drain --component <id> --generation <n>
    refcli --host <h> --port <p> admin migrate --plan <id>
    refcli --host <h> --port <p> admin rollback --plan <id>
    refcli --host <h> --port <p> admin supersede --plan <id>
    refcli --host <h> --port <p> admin retire --component <id> --generation <n>
    refcli --host <h> --port <p> admin enable-feature --feature <id> --plan <id>
    refcli --host <h> --port <p> admin fence --worker <id> --boot <n> [--reason <text>]

`refcoordinator` runs the evolution coordinator (it prints `REF_COORDINATOR_READY port=<n>` and
serves until it receives `stop` on stdin or is terminated). `refworker` runs one runtime
generation of one component; `--generation`, `--protocols`, `--schemas` and `--artifact`
select the generation it implements, so two builds of the same source act as two real runtime
generations. `--crash-after-migration-commit` is a fault-injection switch used by the
crash-consistency proofs.

## Benchmarks

    ./build/benchmarks/ref_benchmarks

Measured on the development machine (MSVC 19.44, Release, x64):

    compatibility lookup, 2 generations, 1 edge            2.3 ns/op
    compatibility lookup, 10 generations, 17 edges         5.9 ns/op
    compatibility lookup, 100 generations, 197 edges      12.1 ns/op
    compatibility lookup, 1000 generations, 1997 edges    18.1 ns/op
    compatibility lookup, 10000 generations, 16384 edges  38.4 ns/op
    protocol negotiation                                 186.2 ns/op
    feature-gate resolution                               71.4 ns/op
    rollback eligibility evaluation                       84.8 ns/op
    authority query (in process)                         477.4 ns/op
    stage advance (rejected repeats included)            866.0 ns/op
    migration plan validation (real file)              22033.5 ns/op
    worker handshake and registration (TCP)           349787.0 ns/op
    durable state encode, 1000 generations (574 KiB)    1.23 ms/op
    durable state decode, 1000 generations (574 KiB)    2.68 ms/op

Compatibility lookup grows logarithmically with fleet size and state encoding stays linear in state
size: there is no full-matrix rebuild, no re-sorting per query and no linear worker lookup.

## CMake install and downstream use

    cmake --install build --prefix <prefix>

The install provides the library, public headers, `RuntimeEvolutionFabricConfig.cmake`,
`RuntimeEvolutionFabricConfigVersion.cmake` and the exported target set:

    find_package(RuntimeEvolutionFabric CONFIG REQUIRED)
    target_link_libraries(my_target PRIVATE SummonSoftwareLabs::RuntimeEvolutionFabric)

A complete downstream project lives in `consumer/` and is built from the installed prefix only:

    cmake -S consumer -B consumer-build -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build consumer-build
    ./consumer-build/ref_consumer

## Genuine limitations

* Administrative authority is a loopback operator flag. The fabric performs no authentication of its
  own; deploy it inside a trusted boundary and front it with your own authentication.
* One coordinator owns the durable structure. There is no coordinator-to-coordinator consensus, so
  coordinator availability is a single point of failure; the recovery path is restart with an
  advanced coordinator epoch and explicit re-registration.
* The built-in component-state schemas (generations 1 to 3) are the fabric's own bounded state model.
  Migrating a third-party application's persisted format requires implementing a transform for it:
  the fabric governs that migration, it does not invent the transform.
* The multiprocess proofs run real processes on one host over loopback TCP. Cross-site and
  hundred-worker fleets are SYNTHETIC and are not claimed as real deployments.
* Evidence is a generation-stamped local record, not a distributed trust system. `EvidenceClass`
  labels provenance honestly, but the fabric does not verify a remote peer's claims cryptographically.
* Integrity digests are deterministic non-cryptographic digests: they detect corruption and
  accidental mismatch, not tampering.
* `REF_ENABLE_ASAN=ON` instruments first-party targets with the MSVC AddressSanitizer, but this
  host's Visual Studio Build Tools installation does not include the ASan runtime component
  (`clang_rt.asan-x86_64.dll`), so the instrumented binaries cannot be loaded here. The strongest
  validation available on this host is the Debug configuration, which enables the checked STL,
  iterator debugging and the coordinator's state-lock reentrancy detector; the full suite passes in
  Debug. Install the "C++ AddressSanitizer" component to run the instrumented configuration.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
