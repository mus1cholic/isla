#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "ai_gateway_client_session.hpp"

namespace {

using namespace std::chrono_literals;
namespace protocol = isla::shared::ai_gateway;

constexpr std::string_view kSmokeHost = "127.0.0.1";
constexpr std::uint16_t kSmokePort = 8080;
constexpr std::string_view kSmokePath = "/";
constexpr std::string_view kSmokeTurnId = "turn_1";
constexpr std::string_view kSmokeMessage = "hello!";
constexpr auto kSmokeOperationTimeout = 20s;
constexpr auto kSmokeReplyTimeout = 20s;

template <typename... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

struct SmokeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::string> session_id;
    std::optional<std::string> reply_text;
    std::optional<absl::Status> terminal_error;
    bool turn_completed = false;
};

void handle_message(SmokeState& state, const protocol::GatewayMessage& message) {
    std::visit(Overloaded{
                   [&state](const protocol::SessionStartedMessage& value) {
                       LOG(INFO) << "Smoke client: session started session_id='" << value.session_id
                                 << "'";
                       {
                           std::lock_guard<std::mutex> lock(state.mutex);
                           state.session_id = value.session_id;
                       }
                       state.cv.notify_all();
                   },
                   [&state](const protocol::TextOutputMessage& value) {
                       LOG(INFO) << "Smoke client: text output turn_id='" << value.turn_id
                                 << "' text='" << value.text << "'";
                       {
                           std::lock_guard<std::mutex> lock(state.mutex);
                           state.reply_text = value.text;
                       }
                       state.cv.notify_all();
                   },
                   [&state](const protocol::TurnCompletedMessage& value) {
                       LOG(INFO) << "Smoke client: turn completed turn_id='" << value.turn_id
                                 << "'";
                       {
                           std::lock_guard<std::mutex> lock(state.mutex);
                           state.turn_completed = true;
                       }
                       state.cv.notify_all();
                   },
                   [](const protocol::SessionEndedMessage& value) {
                       LOG(INFO) << "Smoke client: session ended session_id='" << value.session_id
                                 << "'";
                   },
                   [&state](const protocol::ErrorMessage& value) {
                       const absl::Status status = absl::FailedPreconditionError(
                           "gateway error " + value.code + ": " + value.message);
                       LOG(ERROR) << "Smoke client: gateway error code='" << value.code
                                  << "' message='" << value.message << "'";
                       {
                           std::lock_guard<std::mutex> lock(state.mutex);
                           state.terminal_error = status;
                       }
                       state.cv.notify_all();
                   },
                   [](const auto&) {},
               },
               message);
}

} // namespace

int main() {
    absl::InitializeLog();
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

    SmokeState state;
    isla::client::AiGatewayClientSession session(isla::client::AiGatewayClientConfig{
        .host = std::string(kSmokeHost),
        .port = kSmokePort,
        .path = std::string(kSmokePath),
        .operation_timeout = kSmokeOperationTimeout,
        .on_message =
            [&state](const protocol::GatewayMessage& message) { handle_message(state, message); },
        .on_transport_closed =
            [&state](absl::Status status) {
                if (!status.ok()) {
                    LOG(WARNING) << "Smoke client: transport closed: " << status;
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        state.terminal_error = status;
                    }
                    state.cv.notify_all();
                }
            },
    });

    const absl::Status connect_status = session.ConnectAndStart();
    if (!connect_status.ok()) {
        LOG(ERROR) << "Smoke client: connect/start failed: " << connect_status;
        session.Close();
        return 1;
    }

    const absl::Status send_status =
        session.SendTextInput(std::string(kSmokeTurnId), std::string(kSmokeMessage));
    if (!send_status.ok()) {
        LOG(ERROR) << "Smoke client: send failed: " << send_status;
        session.Close();
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        const bool finished = state.cv.wait_for(lock, kSmokeReplyTimeout, [&state] {
            return state.turn_completed || state.terminal_error.has_value();
        });
        if (!finished) {
            LOG(ERROR) << "Smoke client: timed out waiting for reply";
            session.Close();
            return 1;
        }
        if (state.terminal_error.has_value() && !state.terminal_error->ok()) {
            LOG(ERROR) << "Smoke client: request failed: " << *state.terminal_error;
            session.Close();
            return 1;
        }
        if (!state.reply_text.has_value()) {
            LOG(ERROR) << "Smoke client: turn completed without a text reply";
            session.Close();
            return 1;
        }
    }

    const absl::Status end_status = session.EndSession();
    if (!end_status.ok()) {
        LOG(WARNING) << "Smoke client: session end failed: " << end_status;
    }
    session.Close();
    return 0;
}
