#include "isla/server/openai_responses_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ai_gateway_string_utils.hpp"
#include "isla/server/ai_gateway_logging_utils.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace isla::server::ai_gateway {
namespace {

struct SseParseSummary {
    bool saw_delta = false;
    bool saw_completed = false;
    std::size_t event_count = 0;
};

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

std::string BuildCurlUrl(const OpenAiResponsesClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port) + config.target;
}

bool ContainsForbiddenCurlConfigOrHeaderChar(std::string_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](char ch) { return ch == '\r' || ch == '\n' || ch == '\0'; });
}

absl::Status ValidateCurlConfigOrHeaderValue(std::string_view field_name, std::string_view value) {
    if (ContainsForbiddenCurlConfigOrHeaderChar(value)) {
        return invalid_argument(std::string(field_name) +
                                " must not contain carriage return, newline, or NUL");
    }
    return absl::OkStatus();
}

absl::StatusOr<std::optional<std::string>> ReadOptionalStringField(const nlohmann::json& object,
                                                                   std::string_view field_name) {
    const auto it = object.find(std::string(field_name));
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        return internal_error("openai responses event field '" + std::string(field_name) +
                              "' must be a string");
    }
    return it->get<std::string>();
}

#if defined(_WIN32)
constexpr std::string_view kCurlCommand = "curl.exe";

std::string QuoteWindowsCommandArg(std::string_view value) {
    const bool needs_quotes =
        value.empty() || value.find_first_of(" \t\"") != std::string_view::npos;
    if (!needs_quotes) {
        return std::string(value);
    }

    std::string quoted;
    quoted.push_back('"');
    std::size_t backslash_count = 0;
    for (const char ch : value) {
        if (ch == '\\') {
            ++backslash_count;
            continue;
        }
        if (ch == '"') {
            quoted.append((backslash_count * 2U) + 1U, '\\');
            quoted.push_back('"');
            backslash_count = 0;
            continue;
        }
        quoted.append(backslash_count, '\\');
        backslash_count = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslash_count * 2U, '\\');
    quoted.push_back('"');
    return quoted;
}

absl::StatusOr<std::string> ResolveWindowsCurlExecutablePath() {
    std::array<char, MAX_PATH + 1> system_directory{};
    const UINT system_directory_length =
        GetSystemDirectoryA(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (system_directory_length == 0 || system_directory_length >= system_directory.size()) {
        return absl::UnavailableError("failed to resolve Windows system directory");
    }

    const std::filesystem::path curl_path =
        std::filesystem::path(system_directory.data()) / std::string(kCurlCommand);
    const DWORD attributes = GetFileAttributesA(curl_path.string().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return absl::NotFoundError("failed to locate trusted curl.exe");
    }
    return curl_path.string();
}
#else
constexpr std::string_view kCurlCommand = "curl";

absl::StatusOr<std::string> ResolvePosixCurlExecutablePath() {
    constexpr std::array<std::string_view, 2> kCurlCandidates = {
        "/usr/bin/curl",
        "/bin/curl",
    };

    for (const std::string_view candidate : kCurlCandidates) {
        const std::filesystem::path path(candidate);
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error || !std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto permissions = status.permissions();
        const bool executable =
            (permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none ||
            (permissions & std::filesystem::perms::group_exec) != std::filesystem::perms::none ||
            (permissions & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
        if (executable) {
            return path.string();
        }
    }

    return absl::NotFoundError("failed to locate trusted curl binary");
}
#endif

class ScopedTempFile final {
  public:
    explicit ScopedTempFile(std::filesystem::path path) : path_(std::move(path)) {}

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&&) = default;
    ScopedTempFile& operator=(ScopedTempFile&&) = default;

    [[nodiscard]] static absl::StatusOr<ScopedTempFile> Create(std::string_view suffix);

    ~ScopedTempFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

#if defined(_WIN32)
class ScopedWindowsHandle final {
  public:
    ScopedWindowsHandle() = default;

    explicit ScopedWindowsHandle(HANDLE handle) : handle_(handle) {}

    ~ScopedWindowsHandle() {
        reset();
    }

    ScopedWindowsHandle(const ScopedWindowsHandle&) = delete;
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&) = delete;

    ScopedWindowsHandle(ScopedWindowsHandle&& other) noexcept : handle_(other.release()) {}

    ScopedWindowsHandle& operator=(ScopedWindowsHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const {
        return handle_;
    }

    [[nodiscard]] bool valid() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE* receive() {
        reset();
        return &handle_;
    }

    [[nodiscard]] HANDLE release() {
        HANDLE released = handle_;
        handle_ = nullptr;
        return released;
    }

    void reset(HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_ = nullptr;
};

class ScopedWindowsAcl final {
  public:
    ScopedWindowsAcl() = default;
    ~ScopedWindowsAcl() {
        if (dacl_ != nullptr) {
            LocalFree(dacl_);
        }
    }

    ScopedWindowsAcl(const ScopedWindowsAcl&) = delete;
    ScopedWindowsAcl& operator=(const ScopedWindowsAcl&) = delete;
    ScopedWindowsAcl(ScopedWindowsAcl&& other) noexcept : dacl_(other.dacl_) {
        other.dacl_ = nullptr;
    }
    ScopedWindowsAcl& operator=(ScopedWindowsAcl&& other) noexcept {
        if (this != &other) {
            if (dacl_ != nullptr) {
                LocalFree(dacl_);
            }
            dacl_ = other.dacl_;
            other.dacl_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] static absl::StatusOr<ScopedWindowsAcl> CreateOwnerOnly() {
        ScopedWindowsHandle token_handle;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token_handle.receive())) {
            return absl::UnavailableError("failed to open process token");
        }

        DWORD token_info_size = 0;
        GetTokenInformation(token_handle.get(), TokenUser, nullptr, 0, &token_info_size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_info_size == 0) {
            return absl::UnavailableError("failed to size process token information");
        }

        std::vector<char> token_info(token_info_size);
        if (!GetTokenInformation(token_handle.get(), TokenUser, token_info.data(), token_info_size,
                                 &token_info_size)) {
            return absl::UnavailableError("failed to read process token information");
        }

        const TOKEN_USER* token_user = reinterpret_cast<const TOKEN_USER*>(token_info.data());
        EXPLICIT_ACCESSA access{};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPSTR>(token_user->User.Sid);

        PACL dacl = nullptr;
        const DWORD acl_status = SetEntriesInAclA(1, &access, nullptr, &dacl);
        if (acl_status != ERROR_SUCCESS) {
            return absl::UnavailableError("failed to build owner-only DACL");
        }
        ScopedWindowsAcl scoped;
        scoped.dacl_ = dacl;
        return scoped;
    }

    [[nodiscard]] PACL get() const {
        return dacl_;
    }

  private:
    PACL dacl_ = nullptr;
};

absl::StatusOr<ScopedTempFile> ScopedTempFile::Create(std::string_view suffix) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    std::array<char, MAX_PATH + 1> path_buffer{};
    const UINT result = GetTempFileNameA(directory.string().c_str(), "isl", 0, path_buffer.data());
    if (result == 0) {
        return absl::UnavailableError("failed to create secure temp file");
    }

    static_cast<void>(suffix);
    const std::filesystem::path path(path_buffer.data());
    const absl::StatusOr<ScopedWindowsAcl> owner_only_acl = ScopedWindowsAcl::CreateOwnerOnly();
    if (!owner_only_acl.ok()) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return owner_only_acl.status();
    }

    const DWORD security_status =
        SetNamedSecurityInfoA(const_cast<char*>(path.string().c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, owner_only_acl->get(), nullptr);
    if (security_status != ERROR_SUCCESS) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return absl::UnavailableError("failed to apply owner-only ACL to secure temp file");
    }

    return ScopedTempFile(path);
}
#else
absl::StatusOr<ScopedTempFile> ScopedTempFile::Create(std::string_view suffix) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / ("isla_openai_XXXXXX" + std::string(suffix)))
            .string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');

    const int fd = mkstemps(mutable_pattern.data(), static_cast<int>(suffix.size()));
    if (fd < 0) {
        return absl::UnavailableError("failed to create secure temp file");
    }
    close(fd);
    return ScopedTempFile(std::filesystem::path(mutable_pattern.data()));
}
#endif

absl::StatusOr<std::string> ReadFileToString(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return absl::NotFoundError("failed to open file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return absl::InternalError("failed to read file: " + path.string());
    }
    return buffer.str();
}

absl::Status WriteStringToFile(const std::filesystem::path& path, std::string_view contents) {
#if defined(_WIN32)
    const DWORD flags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE handle = CreateFileA(path.string().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return absl::NotFoundError("failed to open file for write: " + path.string());
    }

    DWORD bytes_written = 0;
    const BOOL success = WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
                                   &bytes_written, nullptr);
    const DWORD close_success = CloseHandle(handle);
    if (!success || bytes_written != contents.size()) {
        return absl::InternalError("failed to write file: " + path.string());
    }
    if (close_success == 0) {
        return absl::InternalError("failed to close file after write: " + path.string());
    }
    return absl::OkStatus();
#else
    const int open_flags = O_WRONLY | O_TRUNC
#ifdef O_NOFOLLOW
                           | O_NOFOLLOW
#endif
        ;
    const int fd = open(path.c_str(), open_flags, 0600);
    if (fd < 0) {
        return absl::NotFoundError("failed to open file for write: " + path.string());
    }

    std::size_t total_written = 0;
    while (total_written < contents.size()) {
        const ssize_t bytes_written =
            write(fd, contents.data() + total_written, contents.size() - total_written);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return absl::InternalError("failed to write file: " + path.string());
        }
        total_written += static_cast<std::size_t>(bytes_written);
    }
    if (close(fd) != 0) {
        return absl::InternalError("failed to close file after write: " + path.string());
    }
    return absl::OkStatus();
#endif
}

struct CurlProcessResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

std::vector<std::string> BuildCurlArgs(const OpenAiResponsesClientConfig& config,
                                       const std::filesystem::path& curl_config_file,
                                       const std::filesystem::path& request_file,
                                       const std::filesystem::path& header_file) {
    std::vector<std::string> args = {
        std::string(kCurlCommand),
        "-q",
        "--config",
        curl_config_file.string(),
        "--silent",
        "--show-error",
        "--no-buffer",
        "--http1.1",
        "--request",
        "POST",
        BuildCurlUrl(config),
        "--header",
        "Content-Type: application/json",
        "--header",
        "Accept: text/event-stream",
        "--header",
        "User-Agent: " + config.user_agent,
    };
    if (config.organization.has_value()) {
        args.emplace_back("--header");
        args.push_back("OpenAI-Organization: " + *config.organization);
    }
    if (config.project.has_value()) {
        args.emplace_back("--header");
        args.push_back("OpenAI-Project: " + *config.project);
    }
    args.emplace_back("--dump-header");
    args.push_back(header_file.string());
    args.emplace_back("--data-binary");
    args.push_back("@" + request_file.string());
    args.emplace_back("--max-time");
    const std::int64_t timeout_ms = config.request_timeout.count();
    const std::int64_t timeout_seconds = std::max<std::int64_t>(1, (timeout_ms + 999) / 1000);
    args.push_back(std::to_string(timeout_seconds));
    return args;
}

std::string EscapeCurlConfigValue(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

#if defined(_WIN32)
absl::StatusOr<std::string> ReadHandleToString(HANDLE handle) {
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD bytes_read = 0;
        const BOOL success = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                      &bytes_read, nullptr);
        if (!success) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                break;
            }
            return absl::UnavailableError("failed to read process pipe");
        }
        if (bytes_read == 0) {
            break;
        }
        output.append(buffer.data(), bytes_read);
    }
    return output;
}

absl::StatusOr<CurlProcessResult> ExecuteCurlProcess(const std::vector<std::string>& args) {
    absl::StatusOr<ScopedTempFile> stderr_file = ScopedTempFile::Create(".stderr");
    if (!stderr_file.ok()) {
        return stderr_file.status();
    }
    const absl::StatusOr<std::string> curl_path = ResolveWindowsCurlExecutablePath();
    if (!curl_path.ok()) {
        return curl_path.status();
    }

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0)) {
        return absl::UnavailableError("failed to create curl stdout pipe");
    }
    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return absl::UnavailableError("failed to mark curl stdout pipe non-inheritable");
    }

    HANDLE stderr_handle = CreateFileA(
        stderr_file->path().string().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security_attributes, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (stderr_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return absl::UnavailableError("failed to create curl stderr file");
    }

    std::string command_line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0U) {
            command_line.push_back(' ');
        }
        command_line += QuoteWindowsCommandArg(i == 0U ? *curl_path : args[i]);
    }
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_handle;

    PROCESS_INFORMATION process_info{};
    const BOOL created =
        CreateProcessA(curl_path->c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info);

    CloseHandle(stdout_write);
    CloseHandle(stderr_handle);

    if (!created) {
        CloseHandle(stdout_read);
        return absl::UnavailableError("failed to spawn curl process");
    }

    const absl::StatusOr<std::string> stdout_text = ReadHandleToString(stdout_read);
    CloseHandle(stdout_read);
    WaitForSingleObject(process_info.hProcess, INFINITE);

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return absl::UnavailableError("failed to read curl exit code");
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    if (!stdout_text.ok()) {
        return stdout_text.status();
    }

    const absl::StatusOr<std::string> stderr_text = ReadFileToString(stderr_file->path());
    return CurlProcessResult{
        .exit_code = static_cast<int>(exit_code),
        .stdout_text = *stdout_text,
        .stderr_text = stderr_text.ok() ? *stderr_text : std::string(),
    };
}
#else
absl::StatusOr<std::string> ReadFdToString(int fd) {
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return absl::UnavailableError("failed to read process pipe");
        }
        output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    }
    return output;
}

absl::StatusOr<CurlProcessResult> ExecuteCurlProcess(const std::vector<std::string>& args) {
    absl::StatusOr<ScopedTempFile> stderr_file = ScopedTempFile::Create(".stderr");
    if (!stderr_file.ok()) {
        return stderr_file.status();
    }
    const absl::StatusOr<std::string> curl_path = ResolvePosixCurlExecutablePath();
    if (!curl_path.ok()) {
        return curl_path.status();
    }

    int stdout_pipe[2] = { -1, -1 };
    if (pipe(stdout_pipe) != 0) {
        return absl::UnavailableError("failed to create curl stdout pipe");
    }

    const int stderr_fd = open(stderr_file->path().c_str(),
                               O_WRONLY | O_TRUNC
#ifdef O_NOFOLLOW
                                   | O_NOFOLLOW
#endif
                               ,
                               0600);
    if (stderr_fd < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return absl::UnavailableError("failed to create curl stderr file");
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stderr_fd);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1U);
    std::vector<std::string> spawned_args = args;
    spawned_args.front() = *curl_path;
    for (std::string& arg : spawned_args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_result =
        posix_spawn(&pid, curl_path->c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdout_pipe[1]);
    close(stderr_fd);

    if (spawn_result != 0) {
        close(stdout_pipe[0]);
        return absl::UnavailableError("failed to spawn curl process");
    }

    const absl::StatusOr<std::string> stdout_text = ReadFdToString(stdout_pipe[0]);
    close(stdout_pipe[0]);

    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) < 0) {
        return absl::UnavailableError("failed to wait for curl process");
    }
    if (!stdout_text.ok()) {
        return stdout_text.status();
    }

    int exit_code = 0;
    if (WIFEXITED(wait_status)) {
        exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        exit_code = 128 + WTERMSIG(wait_status);
    }

    const absl::StatusOr<std::string> stderr_text = ReadFileToString(stderr_file->path());
    return CurlProcessResult{
        .exit_code = exit_code,
        .stdout_text = *stdout_text,
        .stderr_text = stderr_text.ok() ? *stderr_text : std::string(),
    };
}
#endif

std::optional<unsigned int> ParseHttpStatusCode(std::string_view header_text) {
    std::optional<unsigned int> status_code;
    std::size_t cursor = 0;
    while (cursor <= header_text.size()) {
        const std::size_t next_newline = header_text.find('\n', cursor);
        const std::size_t line_end =
            next_newline == std::string_view::npos ? header_text.size() : next_newline;
        std::string line(header_text.substr(cursor, line_end - cursor));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("HTTP/", 0) == 0) {
            const std::size_t first_space = line.find(' ');
            const std::size_t second_space = first_space == std::string::npos
                                                 ? std::string::npos
                                                 : line.find(' ', first_space + 1U);
            if (first_space != std::string::npos) {
                const std::string_view code_text = std::string_view(line).substr(
                    first_space + 1U,
                    (second_space == std::string::npos ? line.size() : second_space) -
                        (first_space + 1U));
                unsigned int parsed_status_code = 0;
                const auto parse_result = std::from_chars(
                    code_text.data(), code_text.data() + code_text.size(), parsed_status_code);
                if (parse_result.ec == std::errc() &&
                    parse_result.ptr == code_text.data() + code_text.size()) {
                    status_code = parsed_status_code;
                }
            }
        }

        if (next_newline == std::string_view::npos) {
            break;
        }
        cursor = next_newline + 1U;
    }
    return status_code;
}

absl::Status MapHttpErrorStatus(unsigned int status_code, std::string message) {
    switch (status_code) {
    case 400:
    case 404:
    case 409:
    case 422:
        return absl::InvalidArgumentError(std::move(message));
    case 401:
        return absl::UnauthenticatedError(std::move(message));
    case 403:
        return absl::PermissionDeniedError(std::move(message));
    case 408:
    case 504:
        return absl::DeadlineExceededError(std::move(message));
    case 429:
    case 500:
    case 502:
    case 503:
        return absl::UnavailableError(std::move(message));
    default:
        return absl::InternalError(std::move(message));
    }
}

std::string ExtractJsonErrorMessage(const nlohmann::json& json) {
    if (json.contains("error") && json["error"].is_object()) {
        const auto& error = json["error"];
        const absl::StatusOr<std::optional<std::string>> message =
            ReadOptionalStringField(error, "message");
        if (message.ok() && message->has_value()) {
            return **message;
        }
    }
    const absl::StatusOr<std::optional<std::string>> message =
        ReadOptionalStringField(json, "message");
    if (message.ok() && message->has_value()) {
        return **message;
    }
    return {};
}

std::string BuildHttpErrorMessage(unsigned int status_code, std::string_view body) {
    std::string message = "openai responses request failed";
    if (!body.empty()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(body);
            const std::string extracted = ExtractJsonErrorMessage(parsed);
            if (!extracted.empty()) {
                return extracted;
            }
        } catch (const std::exception& error) {
            VLOG(1) << "AI gateway openai responses error body was not JSON detail='"
                    << SanitizeForLog(error.what()) << "'";
        }
    }
    return message + " with status " + std::to_string(status_code);
}

std::optional<std::string> ExtractCompletedText(const nlohmann::json& event_json) {
    if (!event_json.contains("response") || !event_json["response"].is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& response = event_json["response"];
    if (!response.contains("output") || !response["output"].is_array()) {
        return std::nullopt;
    }

    std::string text;
    for (const auto& item : response["output"]) {
        if (!item.is_object()) {
            continue;
        }
        if (!item.contains("content") || !item["content"].is_array()) {
            continue;
        }
        for (const auto& part : item["content"]) {
            if (!part.is_object()) {
                continue;
            }
            const absl::StatusOr<std::optional<std::string>> part_type =
                ReadOptionalStringField(part, "type");
            if (!part_type.ok() || part_type->value_or("") != "output_text") {
                continue;
            }
            if (!part.contains("text") || !part["text"].is_string()) {
                continue;
            }
            text += part["text"].get<std::string>();
        }
    }

    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

absl::Status MapProviderEventError(const nlohmann::json& event_json) {
    const absl::StatusOr<std::optional<std::string>> type_field =
        ReadOptionalStringField(event_json, "type");
    if (!type_field.ok()) {
        return type_field.status();
    }
    const std::string type = type_field->value_or("");
    if (type == "error") {
        std::string message = ExtractJsonErrorMessage(event_json);
        if (message.empty()) {
            message = "openai responses stream returned an error event";
        }
        return absl::UnavailableError(message);
    }
    if (type == "response.failed") {
        std::string message = "openai responses request failed";
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const auto& response = event_json["response"];
            if (response.contains("error") && response["error"].is_object()) {
                const std::string extracted = ExtractJsonErrorMessage(response);
                if (!extracted.empty()) {
                    message = extracted;
                }
            }
        }
        return absl::UnavailableError(message);
    }
    if (type == "response.incomplete") {
        std::string message = "openai responses request completed incompletely";
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const auto& response = event_json["response"];
            if (response.contains("incomplete_details") &&
                response["incomplete_details"].is_object()) {
                const auto& details = response["incomplete_details"];
                const absl::StatusOr<std::optional<std::string>> reason =
                    ReadOptionalStringField(details, "reason");
                if (!reason.ok()) {
                    return reason.status();
                }
                if (reason->has_value()) {
                    message = "openai responses incomplete: " + **reason;
                }
            }
        }
        return absl::UnavailableError(message);
    }
    return absl::OkStatus();
}

absl::Status DispatchStreamEvent(const nlohmann::json& event_json, SseParseSummary* summary,
                                 const OpenAiResponsesEventCallback& on_event) {
    const absl::StatusOr<std::optional<std::string>> type_field =
        ReadOptionalStringField(event_json, "type");
    if (!type_field.ok()) {
        return type_field.status();
    }
    const std::string type = type_field->value_or("");
    if (type.empty()) {
        return absl::OkStatus();
    }
    ++summary->event_count;

    absl::Status provider_error = MapProviderEventError(event_json);
    if (!provider_error.ok()) {
        return provider_error;
    }

    if (type == "response.output_text.delta") {
        const absl::StatusOr<std::optional<std::string>> delta =
            ReadOptionalStringField(event_json, "delta");
        if (!delta.ok()) {
            return delta.status();
        }
        summary->saw_delta = true;
        return on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = delta->value_or(""),
        });
    }
    if (type == "response.completed") {
        if (!summary->saw_delta) {
            const std::optional<std::string> completed_text = ExtractCompletedText(event_json);
            if (!completed_text.has_value() || completed_text->empty()) {
                return internal_error(
                    "openai responses completed without any recoverable output text");
            }
            const absl::Status delta_status =
                on_event(OpenAiResponsesTextDeltaEvent{ .text_delta = *completed_text });
            if (!delta_status.ok()) {
                return delta_status;
            }
        }
        summary->saw_completed = true;
        std::string response_id;
        if (event_json.contains("response") && event_json["response"].is_object()) {
            const absl::StatusOr<std::optional<std::string>> id =
                ReadOptionalStringField(event_json["response"], "id");
            if (!id.ok()) {
                return id.status();
            }
            response_id = id->value_or("");
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = std::move(response_id),
        });
    }

    return absl::OkStatus();
}

absl::StatusOr<bool> FlushBufferedSseEvent(std::string* event_name, std::string* data,
                                           SseParseSummary* summary,
                                           const OpenAiResponsesEventCallback& on_event) {
    static_cast<void>(event_name);
    if (data->empty()) {
        event_name->clear();
        return false;
    }
    if (*data == "[DONE]") {
        event_name->clear();
        data->clear();
        return false;
    }

    nlohmann::json event_json;
    try {
        event_json = nlohmann::json::parse(*data);
    } catch (const std::exception& error) {
        LOG(ERROR) << "AI gateway openai responses stream parse failed detail='"
                   << SanitizeForLog(error.what()) << "'";
        return internal_error("openai responses stream contained invalid JSON");
    }

    event_name->clear();
    data->clear();
    absl::Status dispatch_status = DispatchStreamEvent(event_json, summary, on_event);
    if (!dispatch_status.ok()) {
        return dispatch_status;
    }
    return true;
}

absl::StatusOr<SseParseSummary> ParseSseBody(std::string_view body,
                                             const OpenAiResponsesEventCallback& on_event) {
    SseParseSummary summary;
    std::string event_name;
    std::string data;

    std::size_t cursor = 0;
    while (cursor <= body.size()) {
        const std::size_t next_newline = body.find('\n', cursor);
        const std::size_t line_end =
            next_newline == std::string_view::npos ? body.size() : next_newline;
        std::string line(body.substr(cursor, line_end - cursor));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            const absl::StatusOr<bool> flushed =
                FlushBufferedSseEvent(&event_name, &data, &summary, on_event);
            if (!flushed.ok()) {
                return flushed.status();
            }
        } else if (line.starts_with("data:")) {
            const std::string payload = TrimAscii(std::string_view(line).substr(5));
            if (!data.empty()) {
                data.push_back('\n');
            }
            data.append(payload);
        } else if (line.starts_with("event:")) {
            event_name = TrimAscii(std::string_view(line).substr(6));
        }

        if (next_newline == std::string_view::npos) {
            break;
        }
        cursor = next_newline + 1U;
    }

    const absl::StatusOr<bool> trailing_status =
        FlushBufferedSseEvent(&event_name, &data, &summary, on_event);
    if (!trailing_status.ok()) {
        return trailing_status.status();
    }
    if (!summary.saw_completed) {
        return internal_error("openai responses stream ended before completion");
    }
    return summary;
}

absl::StatusOr<std::string> ExecuteCurl(const OpenAiResponsesClientConfig& config,
                                        const std::string& request_json) {
    absl::StatusOr<ScopedTempFile> curl_config_file = ScopedTempFile::Create(".curlrc");
    if (!curl_config_file.ok()) {
        return curl_config_file.status();
    }
    absl::StatusOr<ScopedTempFile> request_file = ScopedTempFile::Create(".json");
    if (!request_file.ok()) {
        return request_file.status();
    }
    absl::StatusOr<ScopedTempFile> header_file = ScopedTempFile::Create(".headers");
    if (!header_file.ok()) {
        return header_file.status();
    }

    const std::string curl_config_contents =
        "header = \"Authorization: Bearer " + EscapeCurlConfigValue(config.api_key) + "\"\n";
    absl::Status write_status = WriteStringToFile(curl_config_file->path(), curl_config_contents);
    if (!write_status.ok()) {
        return write_status;
    }

    write_status = WriteStringToFile(request_file->path(), request_json);
    if (!write_status.ok()) {
        return write_status;
    }

    // NOTICE: The current Windows toolchain in this repository does not provide OpenSSL headers,
    // which blocks a direct Boost.Beast HTTPS implementation. This subprocess curl transport keeps
    // the OpenAI HTTP/SSE integration behind the provider boundary until the toolchain grows a
    // first-class TLS client dependency.
    const absl::StatusOr<CurlProcessResult> curl_result = ExecuteCurlProcess(
        BuildCurlArgs(config, curl_config_file->path(), request_file->path(), header_file->path()));
    if (!curl_result.ok()) {
        LOG(ERROR) << "AI gateway openai responses failed to launch curl host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' detail='" << SanitizeForLog(curl_result.status().message()) << "'";
        return absl::UnavailableError("failed to launch curl for openai responses request");
    }

    const std::string& body = curl_result->stdout_text;
    const absl::StatusOr<std::string> header_text = ReadFileToString(header_file->path());
    if (!header_text.ok()) {
        LOG(ERROR) << "AI gateway openai responses failed to read curl header dump host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' detail='" << SanitizeForLog(header_text.status().message()) << "'";
        return absl::UnavailableError("failed to read openai responses header dump");
    }
    const std::optional<unsigned int> status_code = ParseHttpStatusCode(*header_text);

    if (curl_result->exit_code != 0) {
        LOG(ERROR) << "AI gateway openai responses curl transport failed host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' exit_code=" << curl_result->exit_code << " http_status="
                   << (status_code.has_value() ? std::to_string(*status_code)
                                               : std::string("<none>"))
                   << " stderr='" << SanitizeForLog(curl_result->stderr_text) << "'";
        if (status_code.has_value()) {
            return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body));
        }
        return absl::UnavailableError("openai responses transport command failed");
    }
    if (status_code.has_value() && *status_code != 200U) {
        return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body));
    }
    return body;
}

class CurlOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    explicit CurlOpenAiResponsesClient(OpenAiResponsesClientConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] absl::Status Validate() const override {
        if (!config_.enabled) {
            return invalid_argument("openai responses client is disabled");
        }
        if (config_.api_key.empty()) {
            return invalid_argument("openai responses api_key must not be empty");
        }
        if (absl::Status status =
                ValidateCurlConfigOrHeaderValue("openai responses api_key", config_.api_key);
            !status.ok()) {
            return status;
        }
        if (config_.host.empty()) {
            return invalid_argument("openai responses host must not be empty");
        }
        if (config_.target.empty() || config_.target.front() != '/') {
            return invalid_argument("openai responses target must start with '/'");
        }
        if (config_.scheme != "http" && config_.scheme != "https") {
            return invalid_argument("openai responses scheme must be 'http' or 'https'");
        }
        if (config_.request_timeout <= std::chrono::milliseconds::zero()) {
            return invalid_argument("openai responses request_timeout must be positive");
        }
        if (absl::Status status =
                ValidateCurlConfigOrHeaderValue("openai responses user_agent", config_.user_agent);
            !status.ok()) {
            return status;
        }
        if (config_.organization.has_value()) {
            if (absl::Status status = ValidateCurlConfigOrHeaderValue(
                    "openai responses organization", *config_.organization);
                !status.ok()) {
                return status;
            }
        }
        if (config_.project.has_value()) {
            if (absl::Status status =
                    ValidateCurlConfigOrHeaderValue("openai responses project", *config_.project);
                !status.ok()) {
                return status;
            }
        }
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        absl::Status status = Validate();
        if (!status.ok()) {
            return status;
        }
        if (request.model.empty()) {
            return invalid_argument("openai responses request must include model");
        }
        if (request.user_text.empty()) {
            return invalid_argument("openai responses request must include user_text");
        }

        nlohmann::json body = {
            { "model", request.model },
            { "input", request.user_text },
            { "stream", true },
        };
        if (!request.system_prompt.empty()) {
            body["instructions"] = request.system_prompt;
        }

        VLOG(1) << "AI gateway openai responses dispatching host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target) << "' model='"
                << SanitizeForLog(request.model)
                << "' timeout_ms=" << config_.request_timeout.count()
                << " user_text_bytes=" << request.user_text.size()
                << " system_prompt_present=" << (!request.system_prompt.empty() ? "true" : "false");

        const absl::StatusOr<std::string> response_body = ExecuteCurl(config_, body.dump());
        if (!response_body.ok()) {
            LOG(ERROR) << "AI gateway openai responses request failed host='"
                       << SanitizeForLog(config_.host) << "' target='"
                       << SanitizeForLog(config_.target) << "' detail='"
                       << SanitizeForLog(response_body.status().message()) << "'";
            return response_body.status();
        }
        const absl::StatusOr<SseParseSummary> parse_summary =
            ParseSseBody(*response_body, on_event);
        if (!parse_summary.ok()) {
            return parse_summary.status();
        }
        VLOG(1) << "AI gateway openai responses completed host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target)
                << "' body_bytes=" << response_body->size()
                << " saw_delta=" << (parse_summary->saw_delta ? "true" : "false")
                << " saw_completed=" << (parse_summary->saw_completed ? "true" : "false")
                << " event_count=" << parse_summary->event_count;
        return absl::OkStatus();
    }

  private:
    OpenAiResponsesClientConfig config_;
};

} // namespace

std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config) {
    return std::make_shared<CurlOpenAiResponsesClient>(std::move(config));
}

} // namespace isla::server::ai_gateway
