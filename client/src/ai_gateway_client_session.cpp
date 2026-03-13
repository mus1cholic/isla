#include "ai_gateway_client_session.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>

#include "absl/log/log.h"
#include "absl/status/statusor.h"

namespace isla::client {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace protocol = isla::shared::ai_gateway;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxInboundWebSocketMessageBytes = 64U * 1024U;

absl::Status invalid_argument(std::string_view message) {
    return absl::InvalidArgumentError(std::string(message));
}

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

absl::Status unavailable(std::string_view message) {
    return absl::UnavailableError(std::string(message));
}

#ifdef _WIN32
std::string utf8_from_wide(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
    const int converted =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                            utf8_size, nullptr, nullptr);
    if (converted <= 0) {
        return {};
    }
    utf8.resize(static_cast<std::size_t>(converted));
    return utf8;
}

std::wstring format_message_wide(DWORD error_code) {
    LPWSTR buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageW(flags, nullptr, error_code, 0,
                                      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0 || buffer == nullptr) {
        return {};
    }

    std::wstring message(buffer, size);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.')) {
        message.pop_back();
    }
    return message;
}

std::string error_message_utf8(const boost::system::error_code& error) {
    const std::wstring wide_message = format_message_wide(static_cast<DWORD>(error.value()));
    if (!wide_message.empty()) {
        const std::string utf8_message = utf8_from_wide(wide_message);
        if (!utf8_message.empty()) {
            return utf8_message;
        }
    }
    return error.message();
}
#else
std::string error_message_utf8(const boost::system::error_code& error) {
    return error.message();
}
#endif

template <typename T>
absl::StatusOr<T> await_future(std::future<T>& future, std::chrono::milliseconds timeout,
                               std::string_view action_name) {
    if (future.wait_for(timeout) != std::future_status::ready) {
        return absl::DeadlineExceededError(std::string(action_name) + " timed out");
    }
    return future.get();
}

std::string format_error(std::string_view context, const boost::system::error_code& error) {
    return std::string(context) + ": " + error_message_utf8(error);
}

} // namespace

class AiGatewayClientSession::Impl
    : public std::enable_shared_from_this<AiGatewayClientSession::Impl> {
  public:
    explicit Impl(AiGatewayClientConfig config)
        : config_(std::move(config)), resolver_(std::make_unique<tcp::resolver>(io_context_)),
          websocket_(std::make_unique<websocket::stream<tcp::socket>>(io_context_)) {}

    absl::Status ConnectAndStart(std::optional<std::string> client_session_id) {
        if (config_.host.empty()) {
            return invalid_argument("ai gateway host must be non-empty");
        }
        if (config_.port == 0U) {
            return invalid_argument("ai gateway port must be non-zero");
        }
        if (config_.path.empty() || config_.path.front() != '/') {
            return invalid_argument("ai gateway path must start with '/'");
        }
        if (config_.operation_timeout <= std::chrono::milliseconds::zero()) {
            return invalid_argument("ai gateway operation_timeout must be positive");
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (thread_started_ || io_thread_.joinable()) {
                return failed_precondition("ai gateway session is already connecting or running");
            }
            if (closing_.load()) {
                return failed_precondition("ai gateway session is closing");
            }
            thread_started_ = true;
        }

        io_context_.restart();
        resolver_ = std::make_unique<tcp::resolver>(io_context_);
        websocket_ = std::make_unique<websocket::stream<tcp::socket>>(io_context_);
        work_guard_.emplace(asio::make_work_guard(io_context_));
        io_thread_ = std::thread([self = shared_from_this()] { self->RunEventLoop(); });
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            io_thread_id_ = io_thread_.get_id();
        }

        auto promise = std::make_shared<std::promise<absl::Status>>();
        std::future<absl::Status> future = promise->get_future();
        asio::post(io_context_, [self = shared_from_this(), promise,
                                 client_session_id = std::move(client_session_id)] {
            if (self->closing_.load()) {
                resolve_promise(promise, failed_precondition("ai gateway session is closing"));
                return;
            }
            self->start_promise_ = promise;
            self->DoResolve(std::move(client_session_id));
        });

        const absl::StatusOr<absl::Status> status =
            await_future(future, config_.operation_timeout, "ai gateway connect/start");
        if (!status.ok()) {
            Close();
            return status.status();
        }
        if (!status->ok()) {
            Close();
            return *status;
        }
        return absl::OkStatus();
    }

    absl::Status SendTextInput(std::string turn_id, std::string text) {
        if (turn_id.empty() || text.empty()) {
            return invalid_argument("ai gateway text input requires non-empty turn_id and text");
        }
        return SendMessage(protocol::TextInputMessage{
            .turn_id = std::move(turn_id),
            .text = std::move(text),
        });
    }

    absl::Status RequestTurnCancel(std::string turn_id) {
        if (turn_id.empty()) {
            return invalid_argument("ai gateway turn cancel requires non-empty turn_id");
        }
        return SendMessage(protocol::TurnCancelMessage{
            .turn_id = std::move(turn_id),
        });
    }

    absl::Status EndSession() {
        std::optional<std::string> active_session_id;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!session_started_) {
                return failed_precondition("ai gateway session is not started");
            }
            if (session_ended_) {
                return absl::OkStatus();
            }
            if (session_end_requested_) {
                return failed_precondition("ai gateway session end already requested");
            }
            session_end_requested_ = true;
            active_session_id = session_id_;
        }
        if (!active_session_id.has_value()) {
            return failed_precondition("ai gateway session_id is unavailable");
        }

        auto promise = std::make_shared<std::promise<absl::Status>>();
        std::future<absl::Status> future = promise->get_future();
        const std::string frame =
            protocol::to_json_string(protocol::SessionEndMessage{ *active_session_id });
        asio::post(io_context_, [self = shared_from_this(), promise, frame] {
            if (self->closing_.load()) {
                resolve_promise(promise, failed_precondition("ai gateway session is closing"));
                return;
            }
            self->end_promise_ = promise;
            self->QueueWrite(PendingWrite{
                .frame = frame,
                .completion = nullptr,
            });
        });

        const absl::StatusOr<absl::Status> status =
            await_future(future, config_.operation_timeout, "ai gateway session end");
        if (!status.ok()) {
            return status.status();
        }
        return *status;
    }

    void Close() {
        bool should_join = false;
        bool called_from_io_thread = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            should_join = thread_started_;
            called_from_io_thread = thread_started_ && std::this_thread::get_id() == io_thread_id_;
        }
        if (!should_join && !io_thread_.joinable()) {
            return;
        }

        if (called_from_io_thread) {
            DoClose(failed_precondition("ai gateway client closed by local shutdown"));
            if (io_thread_.joinable()) {
                io_thread_.detach();
            }
            return;
        }

        if (should_join && !io_context_.stopped()) {
            asio::post(io_context_, [self = shared_from_this()] {
                self->DoClose(failed_precondition("ai gateway client closed by local shutdown"));
            });
        }
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    [[nodiscard]] bool is_open() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return websocket_open_;
    }

    [[nodiscard]] std::optional<std::string> session_id() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return session_id_;
    }

  private:
    struct PendingWrite {
        std::string frame;
        std::shared_ptr<std::promise<absl::Status>> completion;
    };

    void RunEventLoop() {
        io_context_.run();
        FinalizeClosedState();
    }

    void FinalizeClosedState() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        thread_started_ = false;
        io_thread_id_ = std::thread::id{};
        websocket_open_ = false;
        session_started_ = false;
        session_end_requested_ = false;
        session_ended_ = false;
        transport_failed_ = false;
        session_id_.reset();
        read_buffer_.consume(read_buffer_.size());
        closing_.store(false);
        transport_closed_notified_ = false;
        write_in_progress_ = false;
        pending_writes_.clear();
        start_promise_.reset();
        end_promise_.reset();
        work_guard_.reset();
    }

    absl::Status SendMessage(const protocol::GatewayMessage& message) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!session_started_) {
                return failed_precondition("ai gateway session is not started");
            }
            if (transport_failed_) {
                return failed_precondition("ai gateway transport is not running");
            }
            if (session_end_requested_ || session_ended_) {
                return failed_precondition("ai gateway session is ending or ended");
            }
            if (closing_.load()) {
                return failed_precondition("ai gateway session is closing");
            }
        }

        auto promise = std::make_shared<std::promise<absl::Status>>();
        std::future<absl::Status> future = promise->get_future();
        const std::string frame = protocol::to_json_string(message);
        asio::post(io_context_, [self = shared_from_this(), promise, frame] {
            if (self->closing_.load()) {
                resolve_promise(promise, failed_precondition("ai gateway session is closing"));
                return;
            }
            self->QueueWrite(PendingWrite{
                .frame = frame,
                .completion = promise,
            });
        });

        const absl::StatusOr<absl::Status> status =
            await_future(future, config_.operation_timeout, "ai gateway send");
        if (!status.ok()) {
            return status.status();
        }
        return *status;
    }

    void DoResolve(std::optional<std::string> client_session_id) {
        resolver_->async_resolve(
            config_.host, std::to_string(config_.port),
            [self = shared_from_this(), client_session_id = std::move(client_session_id)](
                const boost::system::error_code& error,
                const tcp::resolver::results_type& results) mutable {
                if (error) {
                    self->HandleIoFailure(
                        unavailable(format_error("ai gateway resolve failed", error)));
                    return;
                }
                self->DoConnect(results, std::move(client_session_id));
            });
    }

    void DoConnect(const tcp::resolver::results_type& results,
                   std::optional<std::string> client_session_id) {
        asio::async_connect(
            websocket_->next_layer(), results,
            [self = shared_from_this(), client_session_id = std::move(client_session_id)](
                const boost::system::error_code& error, const tcp::endpoint& /*unused*/) mutable {
                if (error) {
                    self->HandleIoFailure(
                        unavailable(format_error("ai gateway connect failed", error)));
                    return;
                }
                self->DoHandshake(std::move(client_session_id));
            });
    }

    void DoHandshake(std::optional<std::string> client_session_id) {
        websocket_->set_option(
            websocket::stream_base::timeout::suggested(beast::role_type::client));
        websocket_->read_message_max(kMaxInboundWebSocketMessageBytes);
        websocket_->async_handshake(
            config_.host + ":" + std::to_string(config_.port), config_.path,
            [self = shared_from_this(), client_session_id = std::move(client_session_id)](
                const boost::system::error_code& error) mutable {
                if (error) {
                    self->HandleIoFailure(
                        unavailable(format_error("ai gateway websocket handshake failed", error)));
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(self->state_mutex_);
                    self->websocket_open_ = true;
                }
                self->StartRead();
                self->QueueWrite(PendingWrite{
                    .frame = protocol::to_json_string(protocol::SessionStartMessage{
                        .client_session_id = std::move(client_session_id),
                    }),
                    .completion = nullptr,
                });
            });
    }

    void StartRead() {
        websocket_->async_read(read_buffer_,
                               [self = shared_from_this()](const boost::system::error_code& error,
                                                           std::size_t bytes_read) {
                                   self->OnRead(error, bytes_read);
                               });
    }

    void OnRead(const boost::system::error_code& error, std::size_t bytes_read) {
        static_cast<void>(bytes_read);
        if (error) {
            bool normal_session_close = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                normal_session_close = session_ended_;
            }
            if (error == websocket::error::closed && normal_session_close) {
                HandleIoFailure(absl::OkStatus());
            } else if (error == websocket::error::closed) {
                HandleIoFailure(failed_precondition("ai gateway websocket closed"));
            } else {
                HandleIoFailure(unavailable(format_error("ai gateway read failed", error)));
            }
            return;
        }
        if (!websocket_->got_text()) {
            HandleIoFailure(invalid_argument("ai gateway sent a non-text websocket frame"));
            return;
        }

        const std::string payload = beast::buffers_to_string(read_buffer_.data());
        read_buffer_.consume(read_buffer_.size());
        const absl::StatusOr<protocol::GatewayMessage> parsed =
            protocol::parse_json_message(payload);
        if (!parsed.ok()) {
            HandleIoFailure(parsed.status());
            return;
        }
        HandleIncomingMessage(*parsed);

        bool keep_reading = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            keep_reading = websocket_open_ && !closing_.load();
        }
        if (keep_reading) {
            StartRead();
        }
    }

    void HandleIncomingMessage(const protocol::GatewayMessage& message) {
        switch (protocol::message_type(message)) {
        case protocol::MessageType::SessionStarted: {
            const auto& started = std::get<protocol::SessionStartedMessage>(message);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                session_started_ = true;
                session_id_ = started.session_id;
            }
            resolve_promise(start_promise_, absl::OkStatus());
            break;
        }
        case protocol::MessageType::SessionEnded: {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_end_requested_ = true;
            session_ended_ = true;
        }
            resolve_promise(end_promise_, absl::OkStatus());
            break;
        case protocol::MessageType::Error: {
            const auto& error_message = std::get<protocol::ErrorMessage>(message);
            const absl::Status status = failed_precondition(
                "ai gateway error " + error_message.code + ": " + error_message.message);
            if (start_promise_ != nullptr) {
                resolve_promise(start_promise_, status);
            } else if (end_promise_ != nullptr && !error_message.turn_id.has_value()) {
                resolve_promise(end_promise_, status);
            }
            break;
        }
        case protocol::MessageType::SessionStart:
        case protocol::MessageType::SessionEnd:
        case protocol::MessageType::TextInput:
        case protocol::MessageType::TextOutput:
        case protocol::MessageType::AudioOutput:
        case protocol::MessageType::TurnCompleted:
        case protocol::MessageType::TurnCancel:
        case protocol::MessageType::TurnCancelled:
            break;
        }

        if (config_.on_message) {
            try {
                config_.on_message(message);
            } catch (const std::exception& error) {
                LOG(ERROR) << "AI gateway client on_message callback threw: " << error.what();
            } catch (...) {
                LOG(ERROR) << "AI gateway client on_message callback threw an unknown exception";
            }
        }
    }

    void QueueWrite(PendingWrite write) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!websocket_open_) {
                resolve_promise(write.completion,
                                failed_precondition("ai gateway websocket is closed"));
                return;
            }
        }

        pending_writes_.push_back(std::move(write));
        if (!write_in_progress_) {
            DoWriteNext();
        }
    }

    void DoWriteNext() {
        if (pending_writes_.empty()) {
            write_in_progress_ = false;
            return;
        }

        write_in_progress_ = true;
        websocket_->text(true);
        websocket_->async_write(asio::buffer(pending_writes_.front().frame.data(),
                                             pending_writes_.front().frame.size()),
                                [self = shared_from_this()](const boost::system::error_code& error,
                                                            std::size_t bytes_written) {
                                    self->OnWrite(error, bytes_written);
                                });
    }

    void OnWrite(const boost::system::error_code& error, std::size_t bytes_written) {
        if (pending_writes_.empty()) {
            write_in_progress_ = false;
            HandleIoFailure(
                absl::InternalError("ai gateway write completed without a pending frame"));
            return;
        }

        PendingWrite write = std::move(pending_writes_.front());
        pending_writes_.pop_front();
        write_in_progress_ = false;

        if (error) {
            const absl::Status status = unavailable(format_error("ai gateway write failed", error));
            resolve_promise(write.completion, status);
            HandleIoFailure(status);
            return;
        }
        if (bytes_written != write.frame.size()) {
            const absl::Status status =
                unavailable("ai gateway websocket write sent a partial frame");
            resolve_promise(write.completion, status);
            HandleIoFailure(status);
            return;
        }

        resolve_promise(write.completion, absl::OkStatus());
        if (!pending_writes_.empty()) {
            DoWriteNext();
        }
    }

    void HandleIoFailure(const absl::Status& status) {
        if (!status.ok()) {
            LOG(WARNING) << "AI gateway client transport failure: " << status;
        }

        resolve_promise(start_promise_, status);
        resolve_promise(end_promise_, status);
        while (!pending_writes_.empty()) {
            resolve_promise(pending_writes_.front().completion, status);
            pending_writes_.pop_front();
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            websocket_open_ = false;
            transport_failed_ = !status.ok();
        }

        if (!transport_closed_notified_ && config_.on_transport_closed) {
            transport_closed_notified_ = true;
            try {
                config_.on_transport_closed(status);
            } catch (const std::exception& error) {
                LOG(ERROR) << "AI gateway client on_transport_closed callback threw: "
                           << error.what();
            } catch (...) {
                LOG(ERROR)
                    << "AI gateway client on_transport_closed callback threw an unknown exception";
            }
        }

        boost::system::error_code ignored;
        resolver_->cancel();
        websocket_->next_layer().cancel(ignored);
        websocket_->next_layer().shutdown(tcp::socket::shutdown_both, ignored);
        websocket_->next_layer().close(ignored);
        work_guard_.reset();
        io_context_.stop();
    }

    void DoClose(const absl::Status& status) {
        if (closing_.exchange(true)) {
            return;
        }
        HandleIoFailure(status);
    }

    static void resolve_promise(const std::shared_ptr<std::promise<absl::Status>>& promise,
                                const absl::Status& status) {
        if (promise == nullptr) {
            return;
        }
        try {
            promise->set_value(status);
        } catch (const std::future_error&) {
        }
    }

    void resolve_promise(std::shared_ptr<std::promise<absl::Status>>& promise,
                         const absl::Status& status) {
        resolve_promise(static_cast<const std::shared_ptr<std::promise<absl::Status>>&>(promise),
                        status);
        promise.reset();
    }

    AiGatewayClientConfig config_;
    asio::io_context io_context_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<tcp::resolver> resolver_;
    std::unique_ptr<websocket::stream<tcp::socket>> websocket_;
    beast::flat_buffer read_buffer_;
    std::thread io_thread_;
    std::thread::id io_thread_id_{};
    std::deque<PendingWrite> pending_writes_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<std::promise<absl::Status>> start_promise_;
    std::shared_ptr<std::promise<absl::Status>> end_promise_;
    std::optional<std::string> session_id_;
    bool thread_started_ = false;
    bool websocket_open_ = false;
    bool session_started_ = false;
    bool session_end_requested_ = false;
    bool session_ended_ = false;
    bool transport_failed_ = false;
    bool write_in_progress_ = false;
    std::atomic<bool> closing_{ false };
    bool transport_closed_notified_ = false;
};

AiGatewayClientSession::AiGatewayClientSession(AiGatewayClientConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}

AiGatewayClientSession::~AiGatewayClientSession() {
    if (impl_ != nullptr) {
        impl_->Close();
    }
}

absl::Status AiGatewayClientSession::ConnectAndStart(std::optional<std::string> client_session_id) {
    return impl_->ConnectAndStart(std::move(client_session_id));
}

absl::Status AiGatewayClientSession::SendTextInput(std::string turn_id, std::string text) {
    return impl_->SendTextInput(std::move(turn_id), std::move(text));
}

absl::Status AiGatewayClientSession::RequestTurnCancel(std::string turn_id) {
    return impl_->RequestTurnCancel(std::move(turn_id));
}

absl::Status AiGatewayClientSession::EndSession() {
    return impl_->EndSession();
}

void AiGatewayClientSession::Close() {
    impl_->Close();
}

bool AiGatewayClientSession::is_open() const {
    return impl_->is_open();
}

std::optional<std::string> AiGatewayClientSession::session_id() const {
    return impl_->session_id();
}

} // namespace isla::client
