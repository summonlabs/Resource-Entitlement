// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "commands.hpp"
#include "kv.hpp"
#include "resource_entitlement/error.hpp"

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      entl::cli::print_usage();
      return entl::cli::kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
      entl::cli::print_usage();
      return entl::cli::kExitOk;
    }
    if (command == "--version" || command == "-V") {
      std::cout << "entl " << "1.0.0" << " (" << (sizeof(void*) == 8u ? "x64" : "x86") << ")\n";
      return entl::cli::kExitOk;
    }
    if (!entl::cli::is_known_command(command)) {
      return entl::cli::report_error(
          entl::Error(entl::ErrorCode::kMalformedInput, "unknown command", command));
    }
    std::vector<std::string> rest;
    rest.reserve(static_cast<std::size_t>(argc >= 2 ? argc - 2 : 0));
    for (int index = 2; index < argc; ++index) {
      rest.emplace_back(argv[index]);
    }
    auto args = entl::cli::Args::parse(std::move(rest));
    if (!args.has_value()) {
      return entl::cli::report_error(args.error());
    }
    return entl::cli::dispatch(command, args.value());
  } catch (const std::exception& error) {
    return entl::cli::report_error(
        entl::Error(entl::ErrorCode::kInternalError, "unhandled exception", error.what()));
  } catch (...) {
    return entl::cli::report_error(
        entl::Error(entl::ErrorCode::kInternalError, "unhandled non-standard exception"));
  }
}
