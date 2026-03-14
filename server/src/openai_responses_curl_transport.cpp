#include "isla/server/openai_responses_curl_transport.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"
#include "isla/server/openai_responses_http_utils.hpp"
#include "isla/server/openai_responses_transport_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

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

std::string BuildCurlUrl(const OpenAiResponsesClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port) + config.target;
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

struct CurlProcess {
    ScopedWindowsHandle process_handle;
    ScopedWindowsHandle thread_handle;
    ScopedWindowsHandle stdout_read;
};

absl::StatusOr<CurlProcess> StartCurlProcess(const std::vector<std::string>& args,
                                             const std::filesystem::path& stderr_path) {
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
        stderr_path.string().c_str(), GENERIC_WRITE,
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

    return CurlProcess{
        .process_handle = ScopedWindowsHandle(process_info.hProcess),
        .thread_handle = ScopedWindowsHandle(process_info.hThread),
        .stdout_read = ScopedWindowsHandle(stdout_read),
    };
}

absl::StatusOr<std::size_t> ReadCurlStdoutChunk(CurlProcess* process, char* buffer,
                                                std::size_t buffer_size, bool* eof) {
    DWORD bytes_read = 0;
    const BOOL success = ReadFile(process->stdout_read.get(), buffer,
                                  static_cast<DWORD>(buffer_size), &bytes_read, nullptr);
    if (!success) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) {
            *eof = true;
            return 0U;
        }
        return absl::UnavailableError("failed to read process pipe");
    }
    *eof = bytes_read == 0;
    return static_cast<std::size_t>(bytes_read);
}

void CloseCurlStdoutPipe(CurlProcess* process) {
    process->stdout_read.reset();
}

void TerminateCurlProcess(CurlProcess* process) {
    if (process->process_handle.valid()) {
        static_cast<void>(TerminateProcess(process->process_handle.get(), 1));
    }
}

absl::StatusOr<int> WaitForCurlProcess(CurlProcess* process) {
    WaitForSingleObject(process->process_handle.get(), INFINITE);

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process->process_handle.get(), &exit_code)) {
        return absl::UnavailableError("failed to read curl exit code");
    }
    return static_cast<int>(exit_code);
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

class ScopedPosixFd final {
  public:
    ScopedPosixFd() = default;
    explicit ScopedPosixFd(int fd) : fd_(fd) {}
    ~ScopedPosixFd() {
        reset();
    }

    ScopedPosixFd(const ScopedPosixFd&) = delete;
    ScopedPosixFd& operator=(const ScopedPosixFd&) = delete;
    ScopedPosixFd(ScopedPosixFd&& other) noexcept : fd_(other.release()) {}
    ScopedPosixFd& operator=(ScopedPosixFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const {
        return fd_;
    }

    [[nodiscard]] bool valid() const {
        return fd_ >= 0;
    }

    [[nodiscard]] int release() {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_ = -1;
};

struct CurlProcess {
    pid_t pid = -1;
    ScopedPosixFd stdout_fd;
};

absl::StatusOr<CurlProcess> StartCurlProcess(const std::vector<std::string>& args,
                                             const std::filesystem::path& stderr_path) {
    const absl::StatusOr<std::string> curl_path = ResolvePosixCurlExecutablePath();
    if (!curl_path.ok()) {
        return curl_path.status();
    }

    int stdout_pipe[2] = { -1, -1 };
    if (pipe(stdout_pipe) != 0) {
        return absl::UnavailableError("failed to create curl stdout pipe");
    }

    const int stderr_fd = open(stderr_path.c_str(),
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

    return CurlProcess{
        .pid = pid,
        .stdout_fd = ScopedPosixFd(stdout_pipe[0]),
    };
}

absl::StatusOr<std::size_t> ReadCurlStdoutChunk(CurlProcess* process, char* buffer,
                                                std::size_t buffer_size, bool* eof) {
    for (;;) {
        const ssize_t bytes_read = read(process->stdout_fd.get(), buffer, buffer_size);
        if (bytes_read == 0) {
            *eof = true;
            return 0U;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return absl::UnavailableError("failed to read process pipe");
        }
        *eof = false;
        return static_cast<std::size_t>(bytes_read);
    }
}

void CloseCurlStdoutPipe(CurlProcess* process) {
    process->stdout_fd.reset();
}

void TerminateCurlProcess(CurlProcess* process) {
    if (process->pid > 0) {
        static_cast<void>(kill(process->pid, SIGKILL));
    }
}

absl::StatusOr<int> WaitForCurlProcess(CurlProcess* process) {
    int wait_status = 0;
    for (;;) {
        if (waitpid(process->pid, &wait_status, 0) >= 0) {
            break;
        }
        if (errno != EINTR) {
            return absl::UnavailableError("failed to wait for curl process");
        }
    }

    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return 128 + WTERMSIG(wait_status);
    }
    return 1;
}
#endif

} // namespace

absl::StatusOr<TransportStreamResult> ExecuteCurl(const OpenAiResponsesClientConfig& config,
                                                  const std::string& request_json,
                                                  const OpenAiResponsesEventCallback& on_event) {
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
    absl::StatusOr<ScopedTempFile> stderr_file = ScopedTempFile::Create(".stderr");
    if (!stderr_file.ok()) {
        return stderr_file.status();
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

    absl::StatusOr<CurlProcess> curl_process = StartCurlProcess(
        BuildCurlArgs(config, curl_config_file->path(), request_file->path(), header_file->path()),
        stderr_file->path());
    if (!curl_process.ok()) {
        LOG(ERROR) << "AI gateway openai responses failed to launch curl host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' detail='" << SanitizeForLog(curl_process.status().message()) << "'";
        return absl::UnavailableError("failed to launch curl for openai responses request");
    }

    CurlProcess process = std::move(*curl_process);
    IncrementalSseParser parser;
    std::array<char, 4096> chunk_buffer{};
    std::string body_text;
    bool completed_early = false;
    bool aborted_early = false;
    absl::Status early_status;

    for (;;) {
        bool eof = false;
        const absl::StatusOr<std::size_t> bytes_read =
            ReadCurlStdoutChunk(&process, chunk_buffer.data(), chunk_buffer.size(), &eof);
        if (!bytes_read.ok()) {
            CloseCurlStdoutPipe(&process);
            TerminateCurlProcess(&process);
            static_cast<void>(WaitForCurlProcess(&process));
            return bytes_read.status();
        }

        if (*bytes_read > 0U) {
            const absl::StatusOr<SseFeedDisposition> disposition =
                ConsumeTransportChunk(config, std::string_view(chunk_buffer.data(), *bytes_read),
                                      &parser, on_event, &body_text);
            if (!disposition.ok()) {
                early_status = disposition.status();
                aborted_early = true;
                break;
            }
            if (*disposition == SseFeedDisposition::kCompleted) {
                completed_early = true;
                break;
            }
        }

        if (eof) {
            break;
        }
    }

    CloseCurlStdoutPipe(&process);
    if (aborted_early || completed_early) {
        TerminateCurlProcess(&process);
        static_cast<void>(WaitForCurlProcess(&process));
        if (aborted_early) {
            return early_status;
        }
        const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
        if (!parse_summary.ok()) {
            return parse_summary.status();
        }
        return TransportStreamResult{
            .parse_summary = *parse_summary,
            .body_bytes = body_text.size(),
        };
    }

    const absl::StatusOr<int> exit_code = WaitForCurlProcess(&process);
    if (!exit_code.ok()) {
        return exit_code.status();
    }

    const absl::StatusOr<std::string> header_text = ReadFileToString(header_file->path());
    if (!header_text.ok()) {
        LOG(ERROR) << "AI gateway openai responses failed to read curl header dump host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' detail='" << SanitizeForLog(header_text.status().message()) << "'";
        return absl::UnavailableError("failed to read openai responses header dump");
    }
    const std::optional<unsigned int> status_code = ParseHttpStatusCode(*header_text);
    const absl::StatusOr<std::string> stderr_text = ReadFileToString(stderr_file->path());

    if (*exit_code != 0) {
        LOG(ERROR) << "AI gateway openai responses curl transport failed host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' exit_code=" << *exit_code << " http_status="
                   << (status_code.has_value() ? std::to_string(*status_code)
                                               : std::string("<none>"))
                   << " stderr='" << SanitizeForLog(stderr_text.ok() ? *stderr_text : std::string())
                   << "'";
        if (status_code.has_value()) {
            return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body_text));
        }
        return absl::UnavailableError("openai responses transport command failed");
    }
    if (status_code.has_value() && *status_code != 200U) {
        return MapHttpErrorStatus(*status_code, BuildHttpErrorMessage(*status_code, body_text));
    }

    const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
    if (!parse_summary.ok()) {
        return parse_summary.status();
    }
    return TransportStreamResult{
        .parse_summary = *parse_summary,
        .body_bytes = body_text.size(),
    };
}

} // namespace isla::server::ai_gateway
