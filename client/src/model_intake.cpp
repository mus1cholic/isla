#include "model_intake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace isla::client::model_intake {
namespace {

constexpr std::string_view kModelsDirectoryName = "models";
constexpr std::string_view kWorkspaceDirEnvVar = "BUILD_WORKSPACE_DIRECTORY";
constexpr std::string_view kConverterCommandEnvVar = "ISLA_PMX_CONVERTER_COMMAND";
constexpr std::string_view kConverterVersionEnvVar = "ISLA_PMX_CONVERTER_VERSION";
constexpr std::string_view kDefaultConverterCommandTemplate =
    "pmx2gltf --input {input} --output {output}";
constexpr std::string_view kDefaultConverterVersion = "auto";
constexpr std::string_view kConvertedSubdirName = ".isla_converted";
constexpr std::string_view kCacheSchemaVersion = "1";

enum class CandidateKind {
    Glb,
    Gltf,
    Pmx,
};

struct CandidateFile {
    std::filesystem::path path;
    CandidateKind kind = CandidateKind::Gltf;
    bool preferred_name = false;
};

struct PmxConversionOutcome {
    bool ok = false;
    bool cache_hit = false;
    std::filesystem::path output_path;
    std::string info;
    std::string warning;
};

struct ConverterArgvBuildResult {
    std::vector<std::string> argv;
    bool missing_input_token = false;
    bool missing_output_token = false;
};

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool has_supported_extension(const std::filesystem::path& path, CandidateKind* out_kind) {
    const std::string ext = to_lower_ascii(path.extension().string());
    if (ext == ".glb") {
        if (out_kind != nullptr) {
            *out_kind = CandidateKind::Glb;
        }
        return true;
    }
    if (ext == ".gltf") {
        if (out_kind != nullptr) {
            *out_kind = CandidateKind::Gltf;
        }
        return true;
    }
    if (ext == ".pmx") {
        if (out_kind != nullptr) {
            *out_kind = CandidateKind::Pmx;
        }
        return true;
    }
    return false;
}

int kind_priority(CandidateKind kind) {
    switch (kind) {
    case CandidateKind::Glb:
        return 0;
    case CandidateKind::Gltf:
        return 1;
    case CandidateKind::Pmx:
        return 2;
    }
    return 3;
}

bool contains_token(std::string_view text, std::string_view token) {
    return text.find(token) != std::string_view::npos;
}

void replace_all(std::string& text, std::string_view token, std::string_view replacement) {
    std::size_t pos = 0U;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), replacement);
        pos += replacement.size();
    }
}

std::vector<std::string> split_command_template(std::string_view command_template) {
    std::vector<std::string> args;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    for (std::size_t i = 0; i < command_template.size(); ++i) {
        const char c = command_template[i];
        if (!in_double_quote && c == '\'') {
            in_single_quote = !in_single_quote;
            continue;
        }
        if (!in_single_quote && c == '"') {
            in_double_quote = !in_double_quote;
            continue;
        }
        const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!in_single_quote && !in_double_quote && is_space) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }
        if (c == '\\' && in_double_quote && (i + 1U) < command_template.size()) {
            const char next = command_template[i + 1U];
            if (next == '"' || next == '\\') {
                current.push_back(next);
                ++i;
                continue;
            }
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    return args;
}

void append_message(std::string& target, std::string_view message, std::string_view separator) {
    if (message.empty()) {
        return;
    }
    if (!target.empty()) {
        target += separator;
    }
    target += message;
}

ConverterArgvBuildResult build_converter_argv(const std::string& command_template,
                                              const std::filesystem::path& input_path,
                                              const std::filesystem::path& output_path) {
    ConverterArgvBuildResult out;
    out.argv = split_command_template(command_template);
    const std::string input = input_path.lexically_normal().string();
    const std::string output = output_path.lexically_normal().string();
    bool has_input_token = false;
    bool has_output_token = false;
    for (std::string& arg : out.argv) {
        if (contains_token(arg, "{input}")) {
            has_input_token = true;
        }
        if (contains_token(arg, "{output}")) {
            has_output_token = true;
        }
        replace_all(arg, "{input}", input);
        replace_all(arg, "{output}", output);
    }
    if (!has_input_token) {
        out.argv.push_back(input);
        out.missing_input_token = true;
    }
    if (!has_output_token) {
        out.argv.push_back(output);
        out.missing_output_token = true;
    }
    return out;
}

std::unordered_map<std::string, std::string>
read_cache_metadata(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return values;
    }
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        values.emplace(line.substr(0U, eq), line.substr(eq + 1U));
    }
    return values;
}

bool write_cache_metadata(const std::filesystem::path& path,
                          const std::unordered_map<std::string, std::string>& values,
                          std::string* warning) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        if (warning != nullptr) {
            *warning = "failed to open conversion cache metadata path '" + path.string() + "'";
        }
        return false;
    }
    const std::array<const char*, 7U> ordered_keys = {
        "schema",          "source_path",       "source_size",
        "source_mtime_ns", "converter_command", "converter_version",
        "output_path",
    };
    for (const char* key : ordered_keys) {
        const auto it = values.find(key);
        if (it == values.end()) {
            continue;
        }
        stream << key << "=" << it->second << "\n";
    }
    if (!stream.good()) {
        if (warning != nullptr) {
            *warning = "failed writing conversion cache metadata to '" + path.string() + "'";
        }
        return false;
    }
    return true;
}

bool is_cache_hit(const std::filesystem::path& cache_path, const std::filesystem::path& output_path,
                  const std::unordered_map<std::string, std::string>& expected) {
    std::error_code output_exists_error;
    if (!std::filesystem::exists(output_path, output_exists_error) || output_exists_error) {
        return false;
    }
    const auto cached = read_cache_metadata(cache_path);
    if (cached.empty()) {
        return false;
    }
    for (const auto& [key, expected_value] : expected) {
        const auto it = cached.find(key);
        if (it == cached.end() || it->second != expected_value) {
            return false;
        }
    }
    return true;
}

std::string describe_cache_miss(const std::filesystem::path& cache_path,
                                const std::filesystem::path& output_path,
                                const std::unordered_map<std::string, std::string>& expected) {
    std::error_code output_exists_error;
    if (!std::filesystem::exists(output_path, output_exists_error) || output_exists_error) {
        return "missing_converted_output";
    }
    const auto cached = read_cache_metadata(cache_path);
    if (cached.empty()) {
        return "missing_or_unreadable_cache_metadata";
    }
    const auto compare_key = [&](const char* key, const char* reason) -> const char* {
        const auto expected_it = expected.find(key);
        if (expected_it == expected.end()) {
            return nullptr;
        }
        const auto cached_it = cached.find(key);
        if (cached_it == cached.end() || cached_it->second != expected_it->second) {
            return reason;
        }
        return nullptr;
    };
    if (const char* reason = compare_key("source_size", "source_size_changed")) {
        return reason;
    }
    if (const char* reason = compare_key("source_mtime_ns", "source_mtime_changed")) {
        return reason;
    }
    if (const char* reason = compare_key("converter_command", "converter_command_changed")) {
        return reason;
    }
    if (const char* reason = compare_key("converter_version", "converter_version_changed")) {
        return reason;
    }
    if (const char* reason = compare_key("schema", "cache_schema_changed")) {
        return reason;
    }
    return "cache_metadata_mismatch";
}

int run_converter_command(std::span<const std::string> argv,
                          const std::function<int(std::span<const std::string>)>& runner) {
    if (runner) {
        return runner(argv);
    }
    if (argv.empty() || argv.front().empty()) {
        return 127;
    }

#if defined(_WIN32)
    std::vector<std::string> args_copy(argv.begin(), argv.end());
    std::vector<char*> arg_ptrs;
    arg_ptrs.reserve(args_copy.size() + 1U);
    for (std::string& arg : args_copy) {
        arg_ptrs.push_back(arg.data());
    }
    arg_ptrs.push_back(nullptr);
    const int rc = _spawnvp(_P_WAIT, arg_ptrs[0], arg_ptrs.data());
    if (rc < 0) {
        return 127;
    }
    return rc;
#else
    std::vector<std::string> args_copy(argv.begin(), argv.end());
    std::vector<char*> arg_ptrs;
    arg_ptrs.reserve(args_copy.size() + 1U);
    for (std::string& arg : args_copy) {
        arg_ptrs.push_back(arg.data());
    }
    arg_ptrs.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        execvp(arg_ptrs[0], arg_ptrs.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return 127;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 127;
#endif
}

std::vector<std::filesystem::path> discover_models_directories() {
    std::vector<std::filesystem::path> dirs;
    std::vector<std::string> seen;
    const auto try_add = [&](const std::filesystem::path& dir) {
        if (dir.empty()) {
            return;
        }
        std::error_code exists_error;
        if (!std::filesystem::exists(dir, exists_error) || exists_error) {
            return;
        }
        const std::string key = to_lower_ascii(dir.lexically_normal().string());
        if (std::ranges::find(seen, key) != seen.end()) {
            return;
        }
        seen.push_back(key);
        dirs.push_back(dir.lexically_normal());
    };

    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (!cwd_error) {
        try_add(cwd / std::string(kModelsDirectoryName));
    }

    const char* workspace_dir = std::getenv(kWorkspaceDirEnvVar.data());
    if (workspace_dir != nullptr && workspace_dir[0] != '\0') {
        try_add(std::filesystem::path(workspace_dir) / std::string(kModelsDirectoryName));
    }
    return dirs;
}

std::vector<CandidateFile> list_candidates_for_directory(const std::filesystem::path& models_dir) {
    std::vector<CandidateFile> preferred;
    const std::array<std::pair<const char*, CandidateKind>, 3U> preferred_names = { {
        { "model.glb", CandidateKind::Glb },
        { "model.gltf", CandidateKind::Gltf },
        { "model.pmx", CandidateKind::Pmx },
    } };
    for (const auto& [name, kind] : preferred_names) {
        const std::filesystem::path candidate = models_dir / name;
        std::error_code exists_error;
        if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
            continue;
        }
        preferred.push_back(CandidateFile{
            .path = candidate.lexically_normal(),
            .kind = kind,
            .preferred_name = true,
        });
    }

    std::vector<CandidateFile> others;
    std::error_code iter_error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(models_dir, iter_error)) {
        if (iter_error) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        CandidateKind kind = CandidateKind::Gltf;
        if (!has_supported_extension(entry.path(), &kind)) {
            continue;
        }
        const std::string filename = to_lower_ascii(entry.path().filename().string());
        if (filename == "model.glb" || filename == "model.gltf" || filename == "model.pmx") {
            continue;
        }
        others.push_back(CandidateFile{
            .path = entry.path().lexically_normal(),
            .kind = kind,
            .preferred_name = false,
        });
    }
    std::ranges::sort(others, [](const CandidateFile& a, const CandidateFile& b) {
        const int ap = kind_priority(a.kind);
        const int bp = kind_priority(b.kind);
        if (ap != bp) {
            return ap < bp;
        }
        return to_lower_ascii(a.path.filename().string()) <
               to_lower_ascii(b.path.filename().string());
    });

    preferred.insert(preferred.end(), others.begin(), others.end());
    return preferred;
}

PmxConversionOutcome convert_pmx_candidate(const CandidateFile& candidate,
                                           const ResolveStartupAssetOptions& options) {
    PmxConversionOutcome outcome;
    const std::filesystem::path source_path = candidate.path;
    const std::filesystem::path converted_dir =
        source_path.parent_path() / std::string(kConvertedSubdirName);
    const std::filesystem::path output_path =
        converted_dir / (source_path.stem().string() + ".auto.glb");
    const std::filesystem::path cache_path =
        converted_dir / (source_path.stem().string() + ".auto.cache");

    std::error_code source_size_error;
    const std::uintmax_t source_size = std::filesystem::file_size(source_path, source_size_error);
    if (source_size_error) {
        outcome.warning = "failed reading PMX source file size for '" + source_path.string() + "'";
        return outcome;
    }
    std::error_code source_time_error;
    const std::filesystem::file_time_type source_mtime =
        std::filesystem::last_write_time(source_path, source_time_error);
    if (source_time_error) {
        outcome.warning = "failed reading PMX source timestamp for '" + source_path.string() + "'";
        return outcome;
    }
    const auto source_mtime_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(source_mtime.time_since_epoch())
            .count();

    std::unordered_map<std::string, std::string> expected_cache = {
        { "schema", std::string(kCacheSchemaVersion) },
        { "source_path", source_path.lexically_normal().string() },
        { "source_size", std::to_string(source_size) },
        { "source_mtime_ns", std::to_string(source_mtime_ns) },
        { "converter_command", options.pmx_converter_command_template },
        { "converter_version", options.pmx_converter_version },
        { "output_path", output_path.lexically_normal().string() },
    };
    if (is_cache_hit(cache_path, output_path, expected_cache)) {
        outcome.ok = true;
        outcome.cache_hit = true;
        outcome.output_path = output_path.lexically_normal();
        outcome.info = "PMX conversion cache hit for '" + source_path.filename().string() +
                       "' output='" + output_path.filename().string() + "'";
        return outcome;
    }
    outcome.info = "PMX conversion cache miss for '" + source_path.filename().string() +
                   "' reason=" + describe_cache_miss(cache_path, output_path, expected_cache);

    std::error_code mkdir_error;
    std::filesystem::create_directories(converted_dir, mkdir_error);
    if (mkdir_error) {
        outcome.warning =
            "failed creating PMX conversion output directory '" + converted_dir.string() + "'";
        return outcome;
    }

    const ConverterArgvBuildResult built_argv =
        build_converter_argv(options.pmx_converter_command_template, source_path, output_path);
    const std::vector<std::string>& argv = built_argv.argv;
    if (argv.empty()) {
        outcome.warning = "PMX conversion command template produced empty argv for '" +
                          source_path.string() + "'";
        return outcome;
    }
    if (built_argv.missing_input_token || built_argv.missing_output_token) {
        std::string template_warning = "PMX conversion command template omitted token(s):";
        if (built_argv.missing_input_token) {
            template_warning += " {input}";
        }
        if (built_argv.missing_output_token) {
            template_warning += " {output}";
        }
        template_warning += "; appended missing path argument(s) positionally";
        append_message(outcome.warning, template_warning, " | ");
    }
    outcome.info += " | invoking converter command template='" +
                    options.pmx_converter_command_template + "' converter_version='" +
                    options.pmx_converter_version + "' executable='" + argv.front() + "'";
    const int exit_code = run_converter_command(argv, options.run_command);
    if (exit_code != 0) {
        outcome.warning = "PMX conversion command failed for '" + source_path.filename().string() +
                          "' exit_code=" + std::to_string(exit_code) + " output_path='" +
                          output_path.string() + "'";
        return outcome;
    }

    std::error_code output_exists_error;
    if (!std::filesystem::exists(output_path, output_exists_error) || output_exists_error) {
        outcome.warning = "PMX conversion command succeeded but output was not produced at '" +
                          output_path.string() + "' source='" + source_path.string() + "'";
        return outcome;
    }

    std::string cache_warning;
    (void)write_cache_metadata(cache_path, expected_cache, &cache_warning);
    append_message(outcome.warning, cache_warning, " | ");
    outcome.ok = true;
    outcome.output_path = output_path.lexically_normal();
    if (outcome.warning.empty()) {
        outcome.info += " | PMX conversion completed for '" + source_path.filename().string() +
                        "' output='" + output_path.filename().string() + "'";
    }
    return outcome;
}

ResolveStartupAssetOptions make_effective_options(const ResolveStartupAssetOptions& options) {
    ResolveStartupAssetOptions effective = options;
    if (effective.pmx_converter_command_template.empty()) {
        const char* env_value = std::getenv(kConverterCommandEnvVar.data());
        if (env_value != nullptr && env_value[0] != '\0') {
            effective.pmx_converter_command_template = env_value;
        }
    }
    if (effective.pmx_converter_version.empty()) {
        const char* env_value = std::getenv(kConverterVersionEnvVar.data());
        if (env_value != nullptr && env_value[0] != '\0') {
            effective.pmx_converter_version = env_value;
        }
    }
    if (effective.pmx_converter_command_template.empty()) {
        effective.pmx_converter_command_template = std::string(kDefaultConverterCommandTemplate);
    }
    if (effective.pmx_converter_version.empty()) {
        effective.pmx_converter_version = std::string(kDefaultConverterVersion);
    }
    return effective;
}

} // namespace

ResolveStartupAssetResult
resolve_startup_asset_from_models(const ResolveStartupAssetOptions& options) {
    ResolveStartupAssetResult result;
    const ResolveStartupAssetOptions effective = make_effective_options(options);
    const std::vector<std::filesystem::path> model_dirs = discover_models_directories();
    if (model_dirs.empty()) {
        result.infos.push_back("no models directory found under current workspace/cwd");
        return result;
    }

    std::vector<CandidateFile> ordered_candidates;
    for (const std::filesystem::path& models_dir : model_dirs) {
        std::vector<CandidateFile> candidates = list_candidates_for_directory(models_dir);
        ordered_candidates.insert(ordered_candidates.end(),
                                  std::make_move_iterator(candidates.begin()),
                                  std::make_move_iterator(candidates.end()));
    }
    if (ordered_candidates.empty()) {
        result.infos.push_back("models directories found but no supported .glb/.gltf/.pmx files");
        return result;
    }

    result.infos.push_back("models candidate count=" + std::to_string(ordered_candidates.size()));
    for (const CandidateFile& candidate : ordered_candidates) {
        if (candidate.kind == CandidateKind::Glb || candidate.kind == CandidateKind::Gltf) {
            result.has_asset = true;
            result.runtime_asset_path = candidate.path.string();
            result.source_label =
                candidate.preferred_name ? "models_preferred_default" : "models_directory_scan";
            result.infos.push_back("selected startup asset '" + candidate.path.filename().string() +
                                   "' (direct glTF/GLB)");
            return result;
        }

        const PmxConversionOutcome conversion = convert_pmx_candidate(candidate, effective);
        if (!conversion.info.empty()) {
            result.infos.push_back(conversion.info);
        }
        if (!conversion.warning.empty()) {
            result.warnings.push_back(conversion.warning);
        }
        if (!conversion.ok) {
            continue;
        }
        result.has_asset = true;
        result.runtime_asset_path = conversion.output_path.string();
        result.source_label = candidate.preferred_name ? "models_preferred_pmx_auto_convert"
                                                       : "models_scan_pmx_auto_convert";
        result.used_pmx_conversion = true;
        result.pmx_conversion_cache_hit = conversion.cache_hit;
        return result;
    }

    result.warnings.emplace_back(
        "no loadable startup model candidate resolved from models directory");
    return result;
}

} // namespace isla::client::model_intake
