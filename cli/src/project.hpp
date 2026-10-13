#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>

namespace wvc::cli {

int cmd_new(const std::string& name, const fs::path& dir);
int cmd_build(Config& cfg, const fs::path& project, bool release, bool skip_frontend);
int cmd_run(Config& cfg, const fs::path& project, bool release);
int cmd_dev(Config& cfg, const fs::path& project);
int cmd_package(Config& cfg, const fs::path& project, const std::string& target);
int cmd_doctor();
int cmd_info(Config& cfg, const fs::path& project);

// Locate the WebViewCpp framework (source tree or install prefix).
fs::path resolve_framework_root(const Config& cfg, const fs::path& project);

// Produce native/wvc_assets.cpp from a packed bundle.
bool generate_embed_source(const fs::path& pack_path, const fs::path& out_cpp,
                           const std::string& label);

}  // namespace wvc::cli
