// Runtime Evolution Fabric - umbrella header.
//
// Vendor-neutral C++20 runtime for governing live evolution of distributed
// runtime components: version compatibility, staged rollout, mixed-version
// coexistence, schema and protocol migration, state transformation, rollback,
// drain, restart boundaries and generation-bound authority without a global
// infrastructure shutdown.
#pragma once

#define REF_FABRIC_VERSION_MAJOR 1
#define REF_FABRIC_VERSION_MINOR 0
#define REF_FABRIC_VERSION_PATCH 0
#define REF_FABRIC_VERSION "1.0.0"

#include "ref/authority.hpp"
#include "ref/client.hpp"
#include "ref/compat.hpp"
#include "ref/component.hpp"
#include "ref/coordinator.hpp"
#include "ref/ids.hpp"
#include "ref/migration.hpp"
#include "ref/plan.hpp"
#include "ref/protocol.hpp"
#include "ref/schema.hpp"
#include "ref/store.hpp"
#include "ref/support.hpp"
#include "ref/worker.hpp"
#include "ref/wire.hpp"
