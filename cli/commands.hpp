// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef RESOURCE_ENTITLEMENT_CLI_COMMANDS_HPP
#define RESOURCE_ENTITLEMENT_CLI_COMMANDS_HPP

#include <string>
#include <vector>

#include "kv.hpp"
#include "resource_entitlement/error.hpp"

namespace entl::cli {

/// Exit codes. 0 success, 1 refusal by the entitlement semantics, 2 usage or
/// input error, 3 durable storage failure.
inline constexpr int kExitOk = 0;
inline constexpr int kExitRefused = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitDurability = 3;

[[nodiscard]] const std::vector<std::string>& command_names();
[[nodiscard]] bool is_known_command(const std::string& name);

int dispatch(const std::string& command, const Args& args);
void print_usage();

/// Prints one refusal as a JSON object and returns its exit code.
int report_error(const Error& error);

}  // namespace entl::cli

#endif  // RESOURCE_ENTITLEMENT_CLI_COMMANDS_HPP
