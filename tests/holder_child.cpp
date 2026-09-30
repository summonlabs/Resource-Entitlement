// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Test helper: opens a store for writing and holds the writer lock until it is
// killed or its hold time expires. Never installed.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "resource_entitlement/store.hpp"

int main(int argc, char** argv) {
  std::string directory;
  std::string ready_path;
  std::uint64_t hold_millis = 60000u;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value_of = [&](const char* flag) {
      return argument.rfind(flag, 0) == 0 ? argument.substr(std::strlen(flag)) : std::string();
    };
    std::string value = value_of("--dir=");
    if (!value.empty()) {
      directory = value;
      continue;
    }
    value = value_of("--ready=");
    if (!value.empty()) {
      ready_path = value;
      continue;
    }
    value = value_of("--hold-millis=");
    if (!value.empty()) {
      hold_millis = std::strtoull(value.c_str(), nullptr, 10);
      continue;
    }
  }
  if (directory.empty() || ready_path.empty()) {
    std::cerr << "usage: --dir=PATH --ready=PATH [--hold-millis=N]" << std::endl;
    return 2;
  }

  entl::StoreOpenOptions options;
  options.directory = std::filesystem::path(directory);
  auto opened = entl::Store::open(options);
  if (!opened.has_value()) {
    std::cerr << "open failed: " << opened.error().to_string() << std::endl;
    return 3;
  }
  {
    std::ofstream ready(ready_path, std::ios::trunc);
    ready << "ready " << opened.value()->commit_seq().value() << "\n";
    ready.flush();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(hold_millis));
  return 0;
}
