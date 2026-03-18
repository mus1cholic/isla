#include "client_app.hpp"

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include <SDL3/SDL.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "client_app_test_hooks.hpp"
#include "isla/server/ai_gateway_server.hpp"
#include "isla/server/ai_gateway_stub_responder.hpp"
#include "isla/server/openai_responses_client.hpp"
#include "server/src/openai_responses_test_utils.hpp"

namespace isla::client {
namespace {

using namespace std::chrono_literals;
using isla::server::ai_gateway::GatewayServer;
using isla::server::ai_gateway::GatewayServerConfig;
using isla::server::ai_gateway::GatewayStubResponder;
using isla::server::ai_gateway::GatewayStubResponderConfig;
using isla::server::ai_gateway::kDefaultMidTermMemoryModel;
using isla::server::ai_gateway::OpenAiResponsesClient;
using isla::server::ai_gateway::OpenAiResponsesCompletedEvent;
using isla::server::ai_gateway::OpenAiResponsesEventCallback;
using isla::server::ai_gateway::OpenAiResponsesRequest;
using isla::server::ai_gateway::OpenAiResponsesTextDeltaEvent;
using isla::server::ai_gateway::SequentialSessionIdGenerator;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

class FakeSdlRuntime final : public ISdlRuntime {
  public:
    std::uint64_t now_ticks_ns = 0U;
    mutable std::vector<SDL_Event> queued_events;
    mutable bool text_input_started = false;

    [[nodiscard]] std::uint64_t get_ticks_ns() const override {
        return now_ticks_ns;
    }

    [[nodiscard]] bool init_video() const override {
        return true;
    }

    void quit() const override {}

    [[nodiscard]] bool has_primary_display() const override {
        return true;
    }

    [[nodiscard]] SDL_Window* create_window(const char* /*title*/, int /*width*/, int /*height*/,
                                            std::uint64_t /*flags*/) const override {
        return nullptr;
    }

    [[nodiscard]] SDL_Renderer* create_renderer(SDL_Window* /*window*/) const override {
        return nullptr;
    }

    void destroy_renderer(SDL_Renderer* /*renderer*/) const override {}
    void destroy_window(SDL_Window* /*window*/) const override {}
    void maximize_window(SDL_Window* /*window*/) const override {}

    [[nodiscard]] bool poll_event(SDL_Event* event) const override {
        if (queued_events.empty()) {
            return false;
        }
        *event = queued_events.front();
        queued_events.erase(queued_events.begin());
        return true;
    }

    [[nodiscard]] const bool* get_keyboard_state(int* key_count) const override {
        if (key_count != nullptr) {
            *key_count = 0;
        }
        return nullptr;
    }

    [[nodiscard]] bool get_window_size_in_pixels(SDL_Window* /*window*/, int* width,
                                                 int* height) const override {
        if (width != nullptr) {
            *width = 1280;
        }
        if (height != nullptr) {
            *height = 720;
        }
        return true;
    }

    [[nodiscard]] bool get_window_size(SDL_Window* /*window*/, int* width,
                                       int* height) const override {
        return get_window_size_in_pixels(nullptr, width, height);
    }

    void set_window_bordered(SDL_Window* /*window*/, bool /*bordered*/) const override {}

    [[nodiscard]] bool set_window_relative_mouse_mode(SDL_Window* /*window*/,
                                                      bool /*enabled*/) const override {
        return true;
    }

    [[nodiscard]] bool start_text_input(SDL_Window* /*window*/) const override {
        text_input_started = true;
        return true;
    }

    void stop_text_input(SDL_Window* /*window*/) const override {
        text_input_started = false;
    }
};

class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name_);
        if (existing != nullptr) {
            had_original_ = true;
            original_ = existing;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_original_) {
            set(original_.c_str());
            return;
        }
#if defined(_WIN32)
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }

  private:
    void set(const char* value) {
#if defined(_WIN32)
        _putenv_s(name_, value != nullptr ? value : "");
#else
        if (value == nullptr || value[0] == '\0') {
            unsetenv(name_);
        } else {
            setenv(name_, value, 1);
        }
#endif
    }

    const char* name_ = nullptr;
    bool had_original_ = false;
    std::string original_;
};

class ScopedTempDir {
  public:
    ScopedTempDir() = default;
    explicit ScopedTempDir(std::filesystem::path path) : path_(std::move(path)) {}
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;
    ScopedTempDir(ScopedTempDir&&) = default;
    ScopedTempDir& operator=(ScopedTempDir&&) = default;

    ~ScopedTempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    static ScopedTempDir Create(std::string_view name) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        if (error) {
            return {};
        }

        const std::filesystem::path candidate =
            base / (std::string(name) + "_" +
                    std::to_string(static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(candidate, error);
        if (error) {
            return {};
        }
        return ScopedTempDir(candidate);
    }

    [[nodiscard]] bool is_valid() const {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

  private:
    std::filesystem::path path_{};
};

class FakeOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        if (request.model == kDefaultMidTermMemoryModel) {
            const absl::Status status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No completed episode boundary."
                })json",
            });
            if (!status.ok()) {
                return status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_client_app_mid_term_test",
            });
        }
        absl::Status status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta = "stub echo: " + isla::server::ai_gateway::test::ExtractLatestPromptLine(
                                              request.user_text),
        });
        if (!status.ok()) {
            return status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = "resp_client_app_test",
        });
    }
};

class CountingOpenAiResponsesClient final : public OpenAiResponsesClient {
  public:
    [[nodiscard]] absl::Status Validate() const override {
        return absl::OkStatus();
    }

    [[nodiscard]] absl::Status
    StreamResponse(const OpenAiResponsesRequest& request,
                   const OpenAiResponsesEventCallback& on_event) const override {
        if (request.model == kDefaultMidTermMemoryModel) {
            const absl::Status status = on_event(OpenAiResponsesTextDeltaEvent{
                .text_delta = R"json({
                    "should_flush": false,
                    "item_id": null,
                    "split_at": null,
                    "reasoning": "No completed episode boundary."
                })json",
            });
            if (!status.ok()) {
                return status;
            }
            return on_event(OpenAiResponsesCompletedEvent{
                .response_id = "resp_client_app_mid_term_counting_test",
            });
        }
        const int call_number = call_count_.fetch_add(1) + 1;
        absl::Status status = on_event(OpenAiResponsesTextDeltaEvent{
            .text_delta =
                "stub echo " + std::to_string(call_number) + ": " +
                isla::server::ai_gateway::test::ExtractLatestPromptLine(request.user_text),
        });
        if (!status.ok()) {
            return status;
        }
        return on_event(OpenAiResponsesCompletedEvent{
            .response_id = "resp_client_app_counting_test",
        });
    }

    [[nodiscard]] int call_count() const {
        return call_count_.load();
    }

  private:
    mutable std::atomic<int> call_count_{ 0 };
};

class RawTcpHandshakeRejectServer {
  public:
    RawTcpHandshakeRejectServer() : acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0)) {
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { Run(); });
    }

    ~RawTcpHandshakeRejectServer() {
        Stop();
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

  private:
    void Stop() {
        if (!stopped_) {
            stopped_ = true;
            boost::system::error_code error;
            acceptor_.close(error);
            io_context_.stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Run() {
        try {
            tcp::socket socket(io_context_);
            acceptor_.accept(socket);
            boost::system::error_code error;
            socket.shutdown(tcp::socket::shutdown_both, error);
            socket.close(error);
        } catch (...) {
        }
    }

    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    bool stopped_ = false;
    std::uint16_t port_ = 0;
};

class ClientAppGatewayIntegrationTest : public ::testing::Test {
  protected:
    ClientAppGatewayIntegrationTest()
        : responder_(GatewayStubResponderConfig{
              .response_delay = 0ms,
              .openai_client = std::make_shared<FakeOpenAiResponsesClient>(),
          }),
          server_(
              GatewayServerConfig{
                  .bind_host = "127.0.0.1",
                  .port = 0,
                  .listen_backlog = 4,
              },
              &responder_, std::make_unique<SequentialSessionIdGenerator>("cli_app_")) {
        responder_.AttachSessionRegistry(&server_.session_registry());
    }

    void SetUp() override {
        ASSERT_TRUE(server_.Start().ok());
        ASSERT_NE(server_.bound_port(), 0);
    }

    void TearDown() override {
        server_.Stop();
    }

    GatewayStubResponder responder_;
    GatewayServer server_;
};

bool PumpUntil(ClientApp& app, std::chrono::milliseconds timeout,
               const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        internal::ClientAppTestHooks::tick(app);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    internal::ClientAppTestHooks::tick(app);
    return predicate();
}

TEST_F(ClientAppGatewayIntegrationTest, HotkeySendsCannedPromptAndReceivesReply) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server_.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "hello from client app")
            .ok());
    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_connected(app));
    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_session_id(app).has_value());

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_last_reply_text(app) ==
                   std::optional<std::string>("stub echo: hello from client app") &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    EXPECT_EQ(internal::ClientAppTestHooks::gateway_last_reply_text(app),
              std::optional<std::string>("stub echo: hello from client app"));
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value());
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_last_error(app).has_value());

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

TEST_F(ClientAppGatewayIntegrationTest, GatewayHudLinesReflectLatestReplyState) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server_.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "hello from hud")
            .ok());

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_last_reply_text(app) ==
                   std::optional<std::string>("stub echo: hello from hud") &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    internal::ClientAppTestHooks::update_debug_overlay(app);
    const std::optional<std::string> session_id =
        internal::ClientAppTestHooks::gateway_session_id(app);
    ASSERT_TRUE(session_id.has_value());

    const std::vector<std::string> expected_lines = {
        "Gateway Debug HUD",
        "Press G to send the canned prompt",
        "Enabled: yes",
        "Connected: yes",
        "Session: " + *session_id,
        "Inflight: <none>",
        "Reply: stub echo: hello from hud",
    };

    EXPECT_EQ(internal::ClientAppTestHooks::debug_overlay_lines(app), expected_lines);

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

TEST_F(ClientAppGatewayIntegrationTest, RendererChatSubmitAddsTranscriptAndAssistantReply) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server_.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "hello from canned prompt")
            .ok());

    internal::ClientAppTestHooks::queue_renderer_chat_submit(app, "hello from chat ui");
    internal::ClientAppTestHooks::tick(app);

    ASSERT_EQ(internal::ClientAppTestHooks::gateway_chat_transcript_lines(app),
              std::vector<std::string>{ "user: hello from chat ui" });
    ASSERT_EQ(internal::ClientAppTestHooks::gateway_inflight_turn_id(app),
              std::optional<std::string>("client_turn_1"));

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        const std::vector<std::string> lines =
            internal::ClientAppTestHooks::gateway_chat_transcript_lines(app);
        return lines == std::vector<std::string>{ "user: hello from chat ui",
                                                  "assistant: stub echo: hello from chat ui" } &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

TEST(ClientAppGatewayStandaloneTest, RepeatedTextOutputForTurnReplacesAssistantTranscriptEntry) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    internal::ClientAppTestHooks::prime_gateway_chat_turn(app, "session_replace", "turn_replace");
    internal::ClientAppTestHooks::process_gateway_message(app,
                                                          shared::ai_gateway::TextOutputMessage{
                                                              .turn_id = "turn_replace",
                                                              .text = "first reply",
                                                          });
    internal::ClientAppTestHooks::process_gateway_message(app,
                                                          shared::ai_gateway::TextOutputMessage{
                                                              .turn_id = "turn_replace",
                                                              .text = "second reply",
                                                          });

    EXPECT_EQ(internal::ClientAppTestHooks::gateway_last_reply_text(app),
              std::optional<std::string>("second reply"));
    EXPECT_EQ(internal::ClientAppTestHooks::gateway_chat_transcript_lines(app),
              std::vector<std::string>{ "assistant: second reply" });
}

TEST_F(ClientAppGatewayIntegrationTest, StartupSkipsGatewayWhenDisabled) {
    ScopedTempDir workspace_dir = ScopedTempDir::Create("isla_client_gateway_disabled");
    ASSERT_TRUE(workspace_dir.is_valid());
    {
        std::ofstream output(workspace_dir.path() / ".env", std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "ISLA_AI_GATEWAY_ENABLED=false\n";
        output << "ISLA_AI_GATEWAY_HOST=127.0.0.1\n";
        output << "ISLA_AI_GATEWAY_PORT=" << server_.bound_port() << "\n";
    }

    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.path().string().c_str());
    ScopedEnvVar enabled_env("ISLA_AI_GATEWAY_ENABLED", "");
    ScopedEnvVar host_env("ISLA_AI_GATEWAY_HOST", "");
    ScopedEnvVar port_env("ISLA_AI_GATEWAY_PORT", "");
    ScopedEnvVar path_env("ISLA_AI_GATEWAY_PATH", "");
    ScopedEnvVar prompt_env("ISLA_AI_GATEWAY_PROMPT", "");

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::initialize_ai_gateway_from_environment(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_connected(app));
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_session_id(app).has_value());

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);
    internal::ClientAppTestHooks::tick(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_last_reply_text(app).has_value());
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value());
    EXPECT_EQ(internal::ClientAppTestHooks::gateway_last_error(app),
              std::optional<std::string>("ai gateway session is not connected"));
    const std::vector<std::string> expected_disabled_lines = {
        "system: Send skipped because the AI gateway session is not connected.",
    };
    EXPECT_EQ(internal::ClientAppTestHooks::gateway_chat_transcript_lines(app),
              expected_disabled_lines);
}

TEST_F(ClientAppGatewayIntegrationTest, LoadsGatewayConfigFromDotEnvFallback) {
    ScopedTempDir workspace_dir = ScopedTempDir::Create("isla_client_gateway_dotenv");
    ASSERT_TRUE(workspace_dir.is_valid());

    {
        std::ofstream output(workspace_dir.path() / ".env", std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "ISLA_AI_GATEWAY_ENABLED=true\n";
        output << "ISLA_AI_GATEWAY_HOST=127.0.0.1\n";
        output << "ISLA_AI_GATEWAY_PORT=" << server_.bound_port() << "\n";
        output << "ISLA_AI_GATEWAY_PATH=/\n";
        output << "ISLA_AI_GATEWAY_PROMPT=hello from dotenv\n";
    }

    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.path().string().c_str());
    ScopedEnvVar enabled_env("ISLA_AI_GATEWAY_ENABLED", "");
    ScopedEnvVar host_env("ISLA_AI_GATEWAY_HOST", "");
    ScopedEnvVar port_env("ISLA_AI_GATEWAY_PORT", "");
    ScopedEnvVar path_env("ISLA_AI_GATEWAY_PATH", "");
    ScopedEnvVar prompt_env("ISLA_AI_GATEWAY_PROMPT", "");

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::initialize_ai_gateway_from_environment(app);

    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_connected(app));
    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_session_id(app).has_value());

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_last_reply_text(app) ==
                   std::optional<std::string>("stub echo: hello from dotenv") &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

TEST_F(ClientAppGatewayIntegrationTest, ProcessEnvOverridesDotEnvGatewayPrompt) {
    ScopedTempDir workspace_dir = ScopedTempDir::Create("isla_client_gateway_env_precedence");
    ASSERT_TRUE(workspace_dir.is_valid());

    {
        std::ofstream output(workspace_dir.path() / ".env", std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "ISLA_AI_GATEWAY_ENABLED=true\n";
        output << "ISLA_AI_GATEWAY_HOST=127.0.0.1\n";
        output << "ISLA_AI_GATEWAY_PORT=" << server_.bound_port() << "\n";
        output << "ISLA_AI_GATEWAY_PATH=/\n";
        output << "ISLA_AI_GATEWAY_PROMPT=hello from dotenv\n";
    }

    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.path().string().c_str());
    const std::string server_port = std::to_string(server_.bound_port());
    ScopedEnvVar enabled_env("ISLA_AI_GATEWAY_ENABLED", "true");
    ScopedEnvVar host_env("ISLA_AI_GATEWAY_HOST", "127.0.0.1");
    ScopedEnvVar port_env("ISLA_AI_GATEWAY_PORT", server_port.c_str());
    ScopedEnvVar path_env("ISLA_AI_GATEWAY_PATH", "/");
    ScopedEnvVar prompt_env("ISLA_AI_GATEWAY_PROMPT", "hello from process env");

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::initialize_ai_gateway_from_environment(app);

    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_connected(app));

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_last_reply_text(app) ==
                   std::optional<std::string>("stub echo: hello from process env") &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

TEST_F(ClientAppGatewayIntegrationTest, StartupFailureLeavesGatewayDisconnected) {
    RawTcpHandshakeRejectServer reject_server;
    ScopedTempDir workspace_dir = ScopedTempDir::Create("isla_client_gateway_startup_failure");
    ASSERT_TRUE(workspace_dir.is_valid());

    {
        std::ofstream output(workspace_dir.path() / ".env", std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "ISLA_AI_GATEWAY_ENABLED=true\n";
        output << "ISLA_AI_GATEWAY_HOST=127.0.0.1\n";
        output << "ISLA_AI_GATEWAY_PORT=" << reject_server.port() << "\n";
        output << "ISLA_AI_GATEWAY_PATH=/\n";
        output << "ISLA_AI_GATEWAY_PROMPT=hello from failed startup\n";
    }

    ScopedEnvVar workspace_env("BUILD_WORKSPACE_DIRECTORY", workspace_dir.path().string().c_str());
    ScopedEnvVar enabled_env("ISLA_AI_GATEWAY_ENABLED", "");
    ScopedEnvVar host_env("ISLA_AI_GATEWAY_HOST", "");
    ScopedEnvVar port_env("ISLA_AI_GATEWAY_PORT", "");
    ScopedEnvVar path_env("ISLA_AI_GATEWAY_PATH", "");
    ScopedEnvVar prompt_env("ISLA_AI_GATEWAY_PROMPT", "");

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    internal::ClientAppTestHooks::initialize_ai_gateway_from_environment(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_connected(app));
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_session_id(app).has_value());

    SDL_Event send_event{};
    send_event.type = SDL_EVENT_KEY_DOWN;
    send_event.key.scancode = SDL_SCANCODE_G;
    send_event.key.repeat = false;
    runtime.queued_events.push_back(send_event);
    internal::ClientAppTestHooks::tick(app);

    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_last_reply_text(app).has_value());
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value());
}

TEST(ClientAppGatewayStandaloneTest,
     InflightTransportCloseAppendsSystemTranscriptEntryAndClearsTurn) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    internal::ClientAppTestHooks::prime_gateway_chat_turn(app, "session_close", "turn_close");
    internal::ClientAppTestHooks::process_gateway_message(app,
                                                          shared::ai_gateway::TextOutputMessage{
                                                              .turn_id = "turn_close",
                                                              .text = "reply before close",
                                                          });

    internal::ClientAppTestHooks::process_gateway_transport_closed(
        app, absl::FailedPreconditionError("ai gateway websocket closed"));

    const std::vector<std::string> lines =
        internal::ClientAppTestHooks::gateway_chat_transcript_lines(app);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "assistant: reply before close");
    EXPECT_TRUE(absl::StartsWith(lines[1], "system: Transport closed: "));
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_connected(app));
    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value());
}

TEST(ClientAppGatewayStandaloneTest, RepeatedHotkeyPressWhileTurnInFlightSendsOnlyOnce) {
    auto counting_client = std::make_shared<CountingOpenAiResponsesClient>();
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 250ms,
        .openai_client = counting_client,
    });
    GatewayServer server(
        GatewayServerConfig{
            .bind_host = "127.0.0.1",
            .port = 0,
            .listen_backlog = 4,
        },
        &responder, std::make_unique<SequentialSessionIdGenerator>("cli_app_repeat_"));
    responder.AttachSessionRegistry(&server.session_registry());
    ASSERT_TRUE(server.Start().ok());

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "hello once")
            .ok());

    SDL_Event first_send{};
    first_send.type = SDL_EVENT_KEY_DOWN;
    first_send.key.scancode = SDL_SCANCODE_G;
    first_send.key.repeat = false;
    SDL_Event second_send = first_send;
    runtime.queued_events.push_back(first_send);
    runtime.queued_events.push_back(second_send);

    internal::ClientAppTestHooks::tick(app);
    ASSERT_EQ(internal::ClientAppTestHooks::gateway_inflight_turn_id(app),
              std::optional<std::string>("client_turn_1"));

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_last_reply_text(app) ==
                   std::optional<std::string>("stub echo 1: hello once") &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));

    EXPECT_EQ(counting_client->call_count(), 1);

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
    server.Stop();
}

TEST(ClientAppGatewayStandaloneTest, RendererChatSubmitWhileTurnInFlightSendsOnlyOnce) {
    auto counting_client = std::make_shared<CountingOpenAiResponsesClient>();
    GatewayStubResponder responder(GatewayStubResponderConfig{
        .response_delay = 250ms,
        .openai_client = counting_client,
    });
    GatewayServer server(
        GatewayServerConfig{
            .bind_host = "127.0.0.1",
            .port = 0,
            .listen_backlog = 4,
        },
        &responder, std::make_unique<SequentialSessionIdGenerator>("cli_app_chat_repeat_"));
    responder.AttachSessionRegistry(&server.session_registry());
    ASSERT_TRUE(server.Start().ok());

    FakeSdlRuntime runtime;
    ClientApp app(runtime);
    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "unused canned prompt")
            .ok());

    internal::ClientAppTestHooks::queue_renderer_chat_submit(app, "hello once");
    internal::ClientAppTestHooks::tick(app);
    ASSERT_EQ(internal::ClientAppTestHooks::gateway_inflight_turn_id(app),
              std::optional<std::string>("client_turn_1"));

    internal::ClientAppTestHooks::queue_renderer_chat_submit(app, "hello twice");
    internal::ClientAppTestHooks::tick(app);
    const std::vector<std::string> expected_inflight_lines = {
        "user: hello once",
    };
    EXPECT_EQ(internal::ClientAppTestHooks::gateway_chat_transcript_lines(app),
              expected_inflight_lines);

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return internal::ClientAppTestHooks::gateway_chat_transcript_lines(app) ==
                   std::vector<std::string>{ "user: hello once",
                                             "assistant: stub echo 1: hello once" } &&
               !internal::ClientAppTestHooks::gateway_inflight_turn_id(app).has_value();
    }));
    EXPECT_EQ(counting_client->call_count(), 1);

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
    server.Stop();
}

TEST_F(ClientAppGatewayIntegrationTest, TransportClosureIsReflectedInGatewayState) {
    FakeSdlRuntime runtime;
    ClientApp app(runtime);

    ASSERT_TRUE(
        internal::ClientAppTestHooks::start_ai_gateway_session(app,
                                                               AiGatewayClientConfig{
                                                                   .host = "127.0.0.1",
                                                                   .port = server_.bound_port(),
                                                                   .path = "/",
                                                                   .operation_timeout = 2s,
                                                               },
                                                               "hello from client app")
            .ok());
    ASSERT_TRUE(internal::ClientAppTestHooks::gateway_connected(app));

    server_.Stop();

    ASSERT_TRUE(PumpUntil(app, 2s, [&] {
        return !internal::ClientAppTestHooks::gateway_connected(app) &&
               internal::ClientAppTestHooks::gateway_last_error(app).has_value();
    }));

    EXPECT_FALSE(internal::ClientAppTestHooks::gateway_connected(app));
    EXPECT_TRUE(internal::ClientAppTestHooks::gateway_last_error(app).has_value());

    internal::ClientAppTestHooks::shutdown_ai_gateway(app);
}

} // namespace
} // namespace isla::client
