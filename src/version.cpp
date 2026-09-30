// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "resource_entitlement/version.hpp"

namespace entl {

const char* version_string() noexcept { return "1.0.0"; }

const char* build_mode_string() noexcept {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

}  // namespace entl
