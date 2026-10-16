#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

// Local players (split-screen). User 0 is always signed in; users 1-3 are
// while a controller is assigned to them (see max_local_users).
bool IsLocalUserSignedIn(uint32_t user_index);
// The profile of a local user (user 0 is the kernel state's own), null for an
// index past 3.
system::xam::UserProfile* ProfileForUser(uint32_t user_index);
// The local user an XUID belongs to, or -1.
int LocalUserOfXuid(uint64_t xuid);

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                       \
  void Register##n##Exports(rex::runtime::ExportResolver* export_resolver, \
                            system::KernelState* kernel_state);
#include "module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace rex
