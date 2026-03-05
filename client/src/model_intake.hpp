#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace isla::client::model_intake {

struct ResolveStartupAssetOptions {
    std::string pmx_converter_command_template;
    std::string pmx_converter_version;
    std::function<int(std::span<const std::string>)> run_command;
};

struct ResolveStartupAssetResult {
    bool has_asset = false;
    std::string runtime_asset_path;
    std::string source_label;
    bool used_pmx_conversion = false;
    bool pmx_conversion_cache_hit = false;
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
};

ResolveStartupAssetResult
resolve_startup_asset_from_models(const ResolveStartupAssetOptions& options = {});

} // namespace isla::client::model_intake
