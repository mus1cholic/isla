#include "isla/server/openai_responses_client.hpp"

#include <algorithm>
#include <array>
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

#include <boost/asio/read_until.hpp>
#if !defined(_WIN32)
#include <boost/asio/ssl.hpp>
#endif
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#if !defined(_WIN32)
#include <boost/beast/ssl.hpp>
#endif
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
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
#if !defined(_WIN32)
namespace ssl = asio::ssl;
#endif

struct SseParseSummary {
    bool saw_delta = false;
    bool saw_completed = false;
    std::size_t event_count = 0;
};

constexpr std::size_t kMaxOpenAiTransportBodyBytes = 256U * 1024U;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status internal_error(std::string_view message) {
    return absl::InternalError(std::string(message));
}

std::string BuildCurlUrl(const OpenAiResponsesClientConfig& config) {
    return config.scheme + "://" + config.host + ":" + std::to_string(config.port) + config.target;
}

bool ContainsForbiddenHttpFieldChar(std::string_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](char ch) { return ch == '\r' || ch == '\n' || ch == '\0'; });
}

absl::Status ValidateHttpFieldValue(std::string_view field_name, std::string_view value) {
    if (ContainsForbiddenHttpFieldChar(value)) {
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

std::optional<unsigned int> ParseHttpStatusCode(std::string_view header_text);

absl::Status MapHttpErrorStatus(unsigned int status_code, std::string message);

std::string BuildHttpErrorMessage(unsigned int status_code, std::string_view body);

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
#else
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
#endif

absl::Status DispatchStreamEvent(const nlohmann::json& event_json, SseParseSummary* summary,
                                 const OpenAiResponsesEventCallback& on_event);

enum class SseFeedDisposition {
    kContinue,
    kCompleted,
};

class IncrementalSseParser final {
  public:
    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    Feed(std::string_view chunk, const OpenAiResponsesEventCallback& on_event) {
        line_buffer_.append(chunk.data(), chunk.size());
        for (;;) {
            const std::size_t newline = line_buffer_.find('\n');
            if (newline == std::string::npos) {
                return SseFeedDisposition::kContinue;
            }

            std::string line = line_buffer_.substr(0, newline);
            line_buffer_.erase(0, newline + 1U);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            const absl::StatusOr<SseFeedDisposition> disposition = ProcessLine(line, on_event);
            if (!disposition.ok()) {
                return disposition.status();
            }
            if (*disposition == SseFeedDisposition::kCompleted) {
                line_buffer_.clear();
                return disposition;
            }
        }
    }

    [[nodiscard]] absl::StatusOr<SseParseSummary>
    Finish(const OpenAiResponsesEventCallback& on_event) {
        if (!line_buffer_.empty()) {
            std::string trailing_line = std::move(line_buffer_);
            line_buffer_.clear();
            if (!trailing_line.empty() && trailing_line.back() == '\r') {
                trailing_line.pop_back();
            }
            const absl::StatusOr<SseFeedDisposition> disposition =
                ProcessLine(trailing_line, on_event);
            if (!disposition.ok()) {
                return disposition.status();
            }
        }

        const absl::StatusOr<SseFeedDisposition> trailing_flush = FlushBufferedEvent(on_event);
        if (!trailing_flush.ok()) {
            return trailing_flush.status();
        }
        if (!summary_.saw_completed) {
            return internal_error("openai responses stream ended before completion");
        }
        return summary_;
    }

  private:
    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    ProcessLine(const std::string& line, const OpenAiResponsesEventCallback& on_event) {
        if (line.empty()) {
            return FlushBufferedEvent(on_event);
        }
        if (line.starts_with("data:")) {
            const std::string payload = TrimAscii(std::string_view(line).substr(5));
            if (!data_.empty()) {
                data_.push_back('\n');
            }
            data_.append(payload);
        } else if (line.starts_with("event:")) {
            event_name_ = TrimAscii(std::string_view(line).substr(6));
        }
        return SseFeedDisposition::kContinue;
    }

    [[nodiscard]] absl::StatusOr<SseFeedDisposition>
    FlushBufferedEvent(const OpenAiResponsesEventCallback& on_event) {
        static_cast<void>(event_name_);
        if (data_.empty()) {
            event_name_.clear();
            return SseFeedDisposition::kContinue;
        }
        if (data_ == "[DONE]") {
            event_name_.clear();
            data_.clear();
            return SseFeedDisposition::kContinue;
        }

        nlohmann::json event_json;
        try {
            event_json = nlohmann::json::parse(data_);
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway openai responses stream parse failed detail='"
                       << SanitizeForLog(error.what()) << "'";
            return internal_error("openai responses stream contained invalid JSON");
        }

        event_name_.clear();
        data_.clear();
        absl::Status dispatch_status = DispatchStreamEvent(event_json, &summary_, on_event);
        if (!dispatch_status.ok()) {
            return dispatch_status;
        }
        if (summary_.saw_completed) {
            return SseFeedDisposition::kCompleted;
        }
        return SseFeedDisposition::kContinue;
    }

    std::string line_buffer_;
    std::string event_name_;
    std::string data_;
    SseParseSummary summary_;
};

#if defined(_WIN32)
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

absl::StatusOr<SseParseSummary> ParseSseBody(std::string_view body,
                                             const OpenAiResponsesEventCallback& on_event) {
    IncrementalSseParser parser;
    const absl::StatusOr<SseFeedDisposition> disposition = parser.Feed(body, on_event);
    if (!disposition.ok()) {
        return disposition.status();
    }
    return parser.Finish(on_event);
}

struct TransportStreamResult {
    SseParseSummary parse_summary;
    std::size_t body_bytes = 0;
};

absl::Status UnavailableTransportError(std::string_view prefix,
                                       const boost::system::error_code& error) {
    return absl::UnavailableError(std::string(prefix) + ": " + error.message());
}

std::string BuildHttpHostHeaderValue(const OpenAiResponsesClientConfig& config) {
    const bool default_http_port = config.scheme == "http" && config.port == 80;
    const bool default_https_port = config.scheme == "https" && config.port == 443;
    if (default_http_port || default_https_port) {
        return config.host;
    }
    return config.host + ":" + std::to_string(config.port);
}

std::string BuildRawHttpRequest(const OpenAiResponsesClientConfig& config,
                                std::string_view request_json) {
    std::ostringstream request;
    request << "POST " << config.target << " HTTP/1.1\r\n";
    request << "Host: " << BuildHttpHostHeaderValue(config) << "\r\n";
    request << "Authorization: Bearer " << config.api_key << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Accept: text/event-stream\r\n";
    request << "User-Agent: " << config.user_agent << "\r\n";
    if (config.organization.has_value()) {
        request << "OpenAI-Organization: " << *config.organization << "\r\n";
    }
    if (config.project.has_value()) {
        request << "OpenAI-Project: " << *config.project << "\r\n";
    }
    request << "Connection: close\r\n";
    request << "Content-Length: " << request_json.size() << "\r\n\r\n";
    request << request_json;
    return request.str();
}

absl::Status AppendTransportBytes(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                                  std::string* body_text) {
    body_text->append(chunk.data(), chunk.size());
    if (body_text->size() > kMaxOpenAiTransportBodyBytes) {
        LOG(ERROR) << "AI gateway openai responses transport body budget exceeded host='"
                   << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                   << "' body_bytes=" << body_text->size()
                   << " budget_bytes=" << kMaxOpenAiTransportBodyBytes;
        return absl::ResourceExhaustedError(
            "openai responses transport body exceeds maximum length");
    }
    return absl::OkStatus();
}

absl::StatusOr<SseFeedDisposition>
ConsumeTransportChunk(const OpenAiResponsesClientConfig& config, std::string_view chunk,
                      IncrementalSseParser* parser, const OpenAiResponsesEventCallback& on_event,
                      std::string* body_text) {
    absl::Status append_status = AppendTransportBytes(config, chunk, body_text);
    if (!append_status.ok()) {
        return append_status;
    }

    const absl::StatusOr<SseFeedDisposition> disposition = parser->Feed(chunk, on_event);
    if (!disposition.ok()) {
        VLOG(1) << "AI gateway openai responses aborting stream on callback/parser status host='"
                << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                << "' status_code=" << static_cast<int>(disposition.status().code()) << " detail='"
                << SanitizeForLog(disposition.status().message()) << "'";
        return disposition.status();
    }
    if (*disposition == SseFeedDisposition::kCompleted) {
        VLOG(1) << "AI gateway openai responses observed terminal completion early host='"
                << SanitizeForLog(config.host) << "' target='" << SanitizeForLog(config.target)
                << "' body_bytes=" << body_text->size();
    }
    return disposition;
}

struct ParsedHttpResponseHead {
    unsigned int status_code = 0;
    std::string buffered_body;
};

void SetInProcessTransportTimeout(beast::tcp_stream* stream,
                                  const OpenAiResponsesClientConfig& config) {
    stream->expires_after(config.request_timeout);
}

#if !defined(_WIN32)
void SetInProcessTransportTimeout(beast::ssl_stream<beast::tcp_stream>* stream,
                                  const OpenAiResponsesClientConfig& config) {
    beast::get_lowest_layer(*stream).expires_after(config.request_timeout);
}
#endif

template <typename Stream>
absl::StatusOr<ParsedHttpResponseHead>
ReadInProcessResponseHead(Stream* stream, const OpenAiResponsesClientConfig& config) {
    asio::streambuf response_buffer;
    boost::system::error_code error;
    SetInProcessTransportTimeout(stream, config);
    asio::read_until(*stream, response_buffer, "\r\n\r\n", error);
    if (error) {
        return UnavailableTransportError("failed to read openai responses HTTP response header",
                                         error);
    }

    std::string response_text;
    {
        std::istream response_stream(&response_buffer);
        response_text.assign(std::istreambuf_iterator<char>(response_stream),
                             std::istreambuf_iterator<char>());
    }
    const std::size_t header_end = response_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return internal_error("openai responses transport response header was incomplete");
    }

    const std::optional<unsigned int> status_code =
        ParseHttpStatusCode(response_text.substr(0, header_end + 4U));
    if (!status_code.has_value()) {
        return internal_error(
            "openai responses transport response did not include a valid HTTP status");
    }

    return ParsedHttpResponseHead{
        .status_code = *status_code,
        .buffered_body = response_text.substr(header_end + 4U),
    };
}

template <typename Stream>
absl::StatusOr<std::size_t>
ReadInProcessBodyChunk(Stream* stream, const OpenAiResponsesClientConfig& config, char* buffer,
                       std::size_t buffer_size, bool* eof) {
    SetInProcessTransportTimeout(stream, config);
    boost::system::error_code error;
    const std::size_t bytes_read = stream->read_some(asio::buffer(buffer, buffer_size), error);
    if (error == asio::error::eof) {
        *eof = true;
        return bytes_read;
    }
    if (error) {
        return UnavailableTransportError("failed to read openai responses HTTP response body",
                                         error);
    }
    return bytes_read;
}

void CloseInProcessStream(beast::tcp_stream* stream) {
    boost::system::error_code ignored;
    stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
}

template <typename Stream>
absl::StatusOr<TransportStreamResult>
ExecuteStreamingResponse(Stream* stream, const OpenAiResponsesClientConfig& config,
                         const std::string& request_json,
                         const OpenAiResponsesEventCallback& on_event) {
    const std::string raw_request = BuildRawHttpRequest(config, request_json);
    boost::system::error_code error;
    SetInProcessTransportTimeout(stream, config);
    asio::write(*stream, asio::buffer(raw_request), error);
    if (error) {
        return UnavailableTransportError("failed to write openai responses HTTP request", error);
    }

    const absl::StatusOr<ParsedHttpResponseHead> response_head =
        ReadInProcessResponseHead(stream, config);
    if (!response_head.ok()) {
        return response_head.status();
    }

    std::string body_text;
    IncrementalSseParser parser;
    if (response_head->status_code == 200U && !response_head->buffered_body.empty()) {
        const absl::StatusOr<SseFeedDisposition> disposition = ConsumeTransportChunk(
            config, response_head->buffered_body, &parser, on_event, &body_text);
        if (!disposition.ok()) {
            return disposition.status();
        }
        if (*disposition == SseFeedDisposition::kCompleted) {
            const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
            if (!parse_summary.ok()) {
                return parse_summary.status();
            }
            return TransportStreamResult{
                .parse_summary = *parse_summary,
                .body_bytes = body_text.size(),
            };
        }
    }

    std::array<char, 4096> chunk_buffer{};
    if (response_head->status_code != 200U) {
        absl::Status append_status =
            AppendTransportBytes(config, response_head->buffered_body, &body_text);
        if (!append_status.ok()) {
            return append_status;
        }
        for (;;) {
            bool eof = false;
            const absl::StatusOr<std::size_t> bytes_read = ReadInProcessBodyChunk(
                stream, config, chunk_buffer.data(), chunk_buffer.size(), &eof);
            if (!bytes_read.ok()) {
                return bytes_read.status();
            }
            if (*bytes_read > 0U) {
                append_status = AppendTransportBytes(
                    config, std::string_view(chunk_buffer.data(), *bytes_read), &body_text);
                if (!append_status.ok()) {
                    return append_status;
                }
            }
            if (eof) {
                break;
            }
        }
        return MapHttpErrorStatus(response_head->status_code,
                                  BuildHttpErrorMessage(response_head->status_code, body_text));
    }

    for (;;) {
        bool eof = false;
        const absl::StatusOr<std::size_t> bytes_read =
            ReadInProcessBodyChunk(stream, config, chunk_buffer.data(), chunk_buffer.size(), &eof);
        if (!bytes_read.ok()) {
            return bytes_read.status();
        }

        if (*bytes_read > 0U) {
            const absl::StatusOr<SseFeedDisposition> disposition =
                ConsumeTransportChunk(config, std::string_view(chunk_buffer.data(), *bytes_read),
                                      &parser, on_event, &body_text);
            if (!disposition.ok()) {
                return disposition.status();
            }
            if (*disposition == SseFeedDisposition::kCompleted) {
                const absl::StatusOr<SseParseSummary> parse_summary = parser.Finish(on_event);
                if (!parse_summary.ok()) {
                    return parse_summary.status();
                }
                return TransportStreamResult{
                    .parse_summary = *parse_summary,
                    .body_bytes = body_text.size(),
                };
            }
        }

        if (eof) {
            break;
        }
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

absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttp(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                     const OpenAiResponsesEventCallback& on_event) {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    beast::tcp_stream stream(io_context);

    boost::system::error_code error;
    stream.expires_after(config.request_timeout);
    const tcp::resolver::results_type endpoints =
        resolver.resolve(config.host, std::to_string(config.port), error);
    if (error) {
        return UnavailableTransportError("failed to resolve openai responses host", error);
    }

    stream.expires_after(config.request_timeout);
    stream.connect(endpoints, error);
    if (error) {
        return UnavailableTransportError("failed to connect to openai responses host", error);
    }

    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&stream, config, request_json, on_event);
    CloseInProcessStream(&stream);
    return result;
}

#if !defined(_WIN32)
absl::Status ConfigureLinuxTlsContext(const OpenAiResponsesClientConfig& config,
                                      ssl::context* context) {
    static_cast<void>(config);
    boost::system::error_code error;
    context->set_default_verify_paths(error);
    if (error) {
        return UnavailableTransportError("failed to load system TLS trust store", error);
    }
    context->set_verify_mode(ssl::verify_peer);
    return absl::OkStatus();
}

absl::Status ConfigureLinuxTlsStream(const OpenAiResponsesClientConfig& config,
                                     beast::ssl_stream<beast::tcp_stream>* stream) {
    if (!SSL_set_tlsext_host_name(stream->native_handle(), config.host.c_str())) {
        return absl::UnavailableError("failed to configure openai responses TLS SNI host");
    }
    stream->set_verify_callback(ssl::host_name_verification(config.host));
    return absl::OkStatus();
}

void CloseInProcessStream(beast::ssl_stream<beast::tcp_stream>* stream) {
    boost::system::error_code ignored;
    stream->shutdown(ignored);
    beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(*stream).socket().close(ignored);
}

absl::StatusOr<TransportStreamResult>
ExecuteInProcessHttps(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                      const OpenAiResponsesEventCallback& on_event) {
    asio::io_context io_context;
    ssl::context context(ssl::context::tls_client);
    absl::Status context_status = ConfigureLinuxTlsContext(config, &context);
    if (!context_status.ok()) {
        return context_status;
    }

    tcp::resolver resolver(io_context);
    beast::ssl_stream<beast::tcp_stream> stream(io_context, context);
    absl::Status stream_status = ConfigureLinuxTlsStream(config, &stream);
    if (!stream_status.ok()) {
        return stream_status;
    }

    boost::system::error_code error;
    beast::get_lowest_layer(stream).expires_after(config.request_timeout);
    const tcp::resolver::results_type endpoints =
        resolver.resolve(config.host, std::to_string(config.port), error);
    if (error) {
        return UnavailableTransportError("failed to resolve openai responses host", error);
    }

    beast::get_lowest_layer(stream).expires_after(config.request_timeout);
    beast::get_lowest_layer(stream).connect(endpoints, error);
    if (error) {
        return UnavailableTransportError("failed to connect to openai responses host", error);
    }

    beast::get_lowest_layer(stream).expires_after(config.request_timeout);
    stream.handshake(ssl::stream_base::client, error);
    if (error) {
        CloseInProcessStream(&stream);
        return UnavailableTransportError("failed to complete openai responses TLS handshake",
                                         error);
    }

    beast::get_lowest_layer(stream).expires_after(config.request_timeout);
    const absl::StatusOr<TransportStreamResult> result =
        ExecuteStreamingResponse(&stream, config, request_json, on_event);
    CloseInProcessStream(&stream);
    return result;
}
#endif

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

    // NOTICE: Native Windows development still uses this curl subprocess transport for HTTPS while
    // the Ubuntu/Linux server path uses the in-process TLS client above. Keep this fallback behind
    // the provider boundary until the local Windows HTTPS path is retired or replaced.
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

absl::StatusOr<TransportStreamResult>
ExecuteTransport(const OpenAiResponsesClientConfig& config, const std::string& request_json,
                 const OpenAiResponsesEventCallback& on_event) {
    if (config.scheme == "http") {
        return ExecuteInProcessHttp(config, request_json, on_event);
    }
#if !defined(_WIN32)
    return ExecuteInProcessHttps(config, request_json, on_event);
#else
    // NOTICE: Native Windows development still uses the curl fallback for HTTPS while the
    // Linux-only server path moves to an in-process TLS client.
    return ExecuteCurl(config, request_json, on_event);
#endif
}

class OpenAiResponsesClientImpl final : public OpenAiResponsesClient {
  public:
    explicit OpenAiResponsesClientImpl(OpenAiResponsesClientConfig config)
        : config_(std::move(config)) {}

    [[nodiscard]] absl::Status Validate() const override {
        if (!config_.enabled) {
            return invalid_argument("openai responses client is disabled");
        }
        if (config_.api_key.empty()) {
            return invalid_argument("openai responses api_key must not be empty");
        }
        if (absl::Status status =
                ValidateHttpFieldValue("openai responses api_key", config_.api_key);
            !status.ok()) {
            return status;
        }
        if (config_.host.empty()) {
            return invalid_argument("openai responses host must not be empty");
        }
        if (absl::Status status = ValidateHttpFieldValue("openai responses host", config_.host);
            !status.ok()) {
            return status;
        }
        if (config_.target.empty() || config_.target.front() != '/') {
            return invalid_argument("openai responses target must start with '/'");
        }
        if (absl::Status status = ValidateHttpFieldValue("openai responses target", config_.target);
            !status.ok()) {
            return status;
        }
        if (config_.scheme != "http" && config_.scheme != "https") {
            return invalid_argument("openai responses scheme must be 'http' or 'https'");
        }
        if (config_.request_timeout <= std::chrono::milliseconds::zero()) {
            return invalid_argument("openai responses request_timeout must be positive");
        }
        if (absl::Status status =
                ValidateHttpFieldValue("openai responses user_agent", config_.user_agent);
            !status.ok()) {
            return status;
        }
        if (config_.organization.has_value()) {
            if (absl::Status status =
                    ValidateHttpFieldValue("openai responses organization", *config_.organization);
                !status.ok()) {
                return status;
            }
        }
        if (config_.project.has_value()) {
            if (absl::Status status =
                    ValidateHttpFieldValue("openai responses project", *config_.project);
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

        const std::string request_json = body.dump();
        const absl::StatusOr<TransportStreamResult> stream_result =
            ExecuteTransport(config_, request_json, on_event);
        if (!stream_result.ok()) {
            LOG(ERROR) << "AI gateway openai responses request failed host='"
                       << SanitizeForLog(config_.host) << "' target='"
                       << SanitizeForLog(config_.target) << "' detail='"
                       << SanitizeForLog(stream_result.status().message()) << "'";
            return stream_result.status();
        }
        VLOG(1) << "AI gateway openai responses completed host='" << SanitizeForLog(config_.host)
                << "' target='" << SanitizeForLog(config_.target)
                << "' body_bytes=" << stream_result->body_bytes
                << " saw_delta=" << (stream_result->parse_summary.saw_delta ? "true" : "false")
                << " saw_completed="
                << (stream_result->parse_summary.saw_completed ? "true" : "false")
                << " event_count=" << stream_result->parse_summary.event_count;
        return absl::OkStatus();
    }

  private:
    OpenAiResponsesClientConfig config_;
};

} // namespace

std::shared_ptr<const OpenAiResponsesClient>
CreateOpenAiResponsesClient(OpenAiResponsesClientConfig config) {
    return std::make_shared<OpenAiResponsesClientImpl>(std::move(config));
}

} // namespace isla::server::ai_gateway
