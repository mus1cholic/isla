#include "isla/server/ai_gateway_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "isla/server/ai_gateway_logging_utils.hpp"

namespace isla::server::ai_gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

absl::Status failed_precondition(std::string_view message) {
    return absl::FailedPreconditionError(std::string(message));
}

std::string format_error(std::string_view context, const boost::system::error_code& error) {
    return std::string(context) + ": " + error.message();
}

std::string format_endpoint(const tcp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

class LiveGatewaySession final : public GatewayLiveSession,
                                 public GatewayWebSocketConnection,
                                 public std::enable_shared_from_this<LiveGatewaySession> {
  public:
    LiveGatewaySession(tcp::socket socket, GatewaySessionRegistry& registry,
                       GatewayWebSocketSessionFactory& factory, std::string remote_endpoint,
                       std::chrono::milliseconds shutdown_write_grace_period)
        : websocket_(std::move(socket)), registry_(registry), factory_(factory),
          remote_endpoint_(std::move(remote_endpoint)), shutdown_timer_(websocket_.get_executor()),
          shutdown_write_grace_period_(shutdown_write_grace_period) {}

    ~LiveGatewaySession() override {
        Join();
    }

    void Start() {
        auto self = shared_from_this();
        asio::post(websocket_.get_executor(), [self] { self->DoAccept(); });
    }

    void Join() {
        std::unique_lock<std::mutex> lock(state_mutex_);
        finished_cv_.wait(lock, [this] { return finished_.load(); });
    }

    void RequestStop() {
        auto self = shared_from_this();
        asio::post(websocket_.get_executor(), [self] {
            self->server_shutdown_requested_.store(true);
            self->ArmShutdownForceCloseTimer();
            self->RequestGracefulClose();
        });
    }

    [[nodiscard]] const std::string& session_id() const override {
        return session_id_;
    }

    [[nodiscard]] bool is_closed() const override {
        return closed_.load();
    }

    void AsyncEmitTextOutput(std::string turn_id, std::string text,
                             GatewayEmitCallback on_complete) override {
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " queueing server-owned text output turn_id=" << SanitizeForLog(turn_id);
        InvokeOnTransport(
            "text.output",
            [this, turn_id = std::move(turn_id), text = std::move(text)] {
                if (adapter_ == nullptr) {
                    return failed_precondition("websocket session is not ready");
                }
                return adapter_->EmitTextOutput(turn_id, text);
            },
            std::move(on_complete));
    }

    void AsyncEmitAudioOutput(std::string turn_id, std::string mime_type, std::string audio_base64,
                              GatewayEmitCallback on_complete) override {
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " queueing server-owned audio output turn_id=" << SanitizeForLog(turn_id)
                << " mime_type=" << SanitizeForLog(mime_type);
        InvokeOnTransport(
            "audio.output",
            [this, turn_id = std::move(turn_id), mime_type = std::move(mime_type),
             audio_base64 = std::move(audio_base64)] {
                if (adapter_ == nullptr) {
                    return failed_precondition("websocket session is not ready");
                }
                return adapter_->EmitAudioOutput(turn_id, mime_type, audio_base64);
            },
            std::move(on_complete));
    }

    void AsyncEmitTurnCompleted(std::string turn_id, GatewayEmitCallback on_complete) override {
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " queueing server-owned turn completed turn_id=" << SanitizeForLog(turn_id);
        InvokeOnTransport(
            "turn.completed",
            [this, turn_id = std::move(turn_id)] {
                if (adapter_ == nullptr) {
                    return failed_precondition("websocket session is not ready");
                }
                return adapter_->EmitTurnCompleted(turn_id);
            },
            std::move(on_complete));
    }

    void AsyncEmitTurnCancelled(std::string turn_id, GatewayEmitCallback on_complete) override {
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " queueing server-owned turn cancelled turn_id=" << SanitizeForLog(turn_id);
        InvokeOnTransport(
            "turn.cancelled",
            [this, turn_id = std::move(turn_id)] {
                if (adapter_ == nullptr) {
                    return failed_precondition("websocket session is not ready");
                }
                return adapter_->EmitTurnCancelled(turn_id);
            },
            std::move(on_complete));
    }

    void AsyncEmitError(std::optional<std::string> turn_id, std::string code, std::string message,
                        GatewayEmitCallback on_complete) override {
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " queueing server-owned error code=" << SanitizeForLog(code) << " turn_id='"
                << (turn_id.has_value() ? SanitizeForLog(*turn_id) : std::string("<none>")) << "'";
        InvokeOnTransport(
            "error",
            [this, turn_id = std::move(turn_id), code = std::move(code),
             message = std::move(message)] {
                if (adapter_ == nullptr) {
                    return failed_precondition("websocket session is not ready");
                }
                const std::optional<std::string_view> turn_id_view =
                    turn_id ? std::optional<std::string_view>(*turn_id) : std::nullopt;
                return adapter_->EmitError(turn_id_view, code, message);
            },
            std::move(on_complete));
    }

    [[nodiscard]] absl::Status SendTextFrame(std::string_view frame) override {
        if (closed_.load()) {
            return failed_precondition("websocket session is closed");
        }
        return EnqueueWrite(std::string(frame));
    }

    void Close(GatewayTransportCloseMode mode) override {
        auto self = shared_from_this();
        asio::dispatch(websocket_.get_executor(), [self, mode] {
            if (mode == GatewayTransportCloseMode::Graceful) {
                self->RequestGracefulClose();
                return;
            }
            self->DoForceCloseTransport();
        });
    }

  private:
    static void CompleteEmitCallback(std::string_view session_id, std::string_view operation,
                                     GatewayEmitCallback& on_complete, absl::Status status) {
        if (!on_complete) {
            return;
        }

        try {
            on_complete(std::move(status));
        } catch (const std::exception& error) {
            LOG(ERROR) << "AI gateway session=" << SanitizeForLog(session_id)
                       << " async emit callback threw op=" << operation << " detail='"
                       << SanitizeForLog(error.what()) << "'";
        } catch (...) {
            LOG(ERROR) << "AI gateway session=" << SanitizeForLog(session_id)
                       << " async emit callback threw op=" << operation
                       << " detail='unknown exception'";
        }
    }

    template <typename Fn>
    void InvokeOnTransport(std::string_view operation, Fn&& fn, GatewayEmitCallback on_complete) {
        if (closed_.load()) {
            auto self = shared_from_this();
            asio::post(websocket_.get_executor(), [self, operation = std::string(operation),
                                                   on_complete = std::move(on_complete)]() mutable {
                LOG(WARNING) << "AI gateway session=" << SanitizeForLog(self->session_id_)
                             << " rejected async emit op=" << operation << " detail='"
                             << SanitizeForLog("websocket session is closed") << "'";
                CompleteEmitCallback(self->session_id_, operation, on_complete,
                                     failed_precondition("websocket session is closed"));
            });
            return;
        }

        auto self = shared_from_this();
        asio::post(websocket_.get_executor(), [self, operation = std::string(operation),
                                               fn = std::forward<Fn>(fn),
                                               on_complete = std::move(on_complete)]() mutable {
            absl::Status status;
            try {
                status = fn();
            } catch (const std::exception& error) {
                status = absl::InternalError(error.what());
            } catch (...) {
                status = absl::InternalError("unknown exception during transport invocation");
            }
            if (!status.ok()) {
                LOG(WARNING) << "AI gateway session=" << SanitizeForLog(self->session_id_)
                             << " async emit failed op=" << operation << " detail='"
                             << SanitizeForLog(status.message()) << "'";
            }
            CompleteEmitCallback(self->session_id_, operation, on_complete, std::move(status));
        });
    }

    [[nodiscard]] absl::Status EnqueueWrite(std::string frame) {
        if (transport_closed_) {
            return failed_precondition("websocket session transport is closed");
        }
        if (close_after_writes_) {
            return failed_precondition("websocket session is closing");
        }

        pending_writes_.push_back(std::move(frame));
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " enqueued websocket text frame pending_writes=" << pending_writes_.size();
        if (!write_in_progress_) {
            DoWriteNext();
        }
        return absl::OkStatus();
    }

    void DoAccept() {
        if (transport_closed_) {
            FinishSession();
            return;
        }

        accept_in_progress_ = true;
        websocket_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        websocket_.read_message_max(kMaxInboundWebSocketMessageBytes);
        auto self = shared_from_this();
        websocket_.async_accept(
            [self](const boost::system::error_code& error) { self->OnAccept(error); });
    }

    void OnAccept(const boost::system::error_code& error) {
        accept_in_progress_ = false;
        if (error) {
            if (transport_force_closed_.load() || server_shutdown_requested_.load()) {
                VLOG(1) << "AI gateway handshake interrupted by server stop remote="
                        << remote_endpoint_ << " detail='" << SanitizeForLog(error.message())
                        << "'";
            } else {
                LOG(WARNING) << "AI gateway rejected websocket handshake remote="
                             << remote_endpoint_ << " detail='" << SanitizeForLog(error.message())
                             << "'";
            }
            DoCloseTransport();
            return;
        }

        websocket_established_ = true;
        adapter_ = factory_.CreateSession(*this, &registry_);
        if (adapter_ == nullptr) {
            LOG(ERROR) << "AI gateway could not create a websocket session adapter";
            RequestGracefulClose();
            return;
        }

        session_id_ = adapter_->session_id();
        registry_.RegisterSession(shared_from_this());
        VLOG(1) << "AI gateway accepted websocket transport remote=" << remote_endpoint_
                << " session=" << SanitizeForLog(session_id_);
        StartRead();
    }

    void StartRead() {
        if (transport_closed_ || adapter_ == nullptr || adapter_->is_closed()) {
            MaybeFinish();
            return;
        }

        read_in_progress_ = true;
        auto self = shared_from_this();
        websocket_.async_read(read_buffer_, [self](const boost::system::error_code& error,
                                                   const std::size_t bytes_read) {
            self->OnRead(error, bytes_read);
        });
    }

    void OnRead(const boost::system::error_code& error, std::size_t bytes_read) {
        read_in_progress_ = false;
        static_cast<void>(bytes_read);

        if (error) {
            if (adapter_ != nullptr) {
                if (transport_force_closed_.load() || server_shutdown_requested_.load()) {
                    VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                            << " transport closed by server stop remote=" << remote_endpoint_;
                    adapter_->HandleServerShutdown();
                    transport_closed_ = true;
                } else if (error == websocket::error::closed) {
                    adapter_->HandleTransportClosed();
                    transport_closed_ = true;
                } else if (error == websocket::error::message_too_big ||
                           error == websocket::error::buffer_overflow) {
                    const absl::Status status =
                        adapter_->HandleTransportError("websocket message too large");
                    if (!status.ok() && !adapter_->is_closed()) {
                        LOG(WARNING) << "AI gateway session=" << SanitizeForLog(session_id_)
                                     << " failed handling oversized message detail='"
                                     << SanitizeForLog(status.message()) << "'";
                    }
                } else {
                    const absl::Status status = adapter_->HandleTransportError(error.message());
                    if (!status.ok() && !adapter_->is_closed()) {
                        LOG(WARNING) << "AI gateway session=" << SanitizeForLog(session_id_)
                                     << " failed handling transport error detail='"
                                     << SanitizeForLog(status.message()) << "'";
                    }
                }
            }
            MaybeFinish();
            return;
        }

        if (!websocket_.got_text()) {
            if (adapter_ != nullptr) {
                const absl::Status status =
                    adapter_->HandleTransportError("unsupported websocket opcode");
                if (!status.ok() && !adapter_->is_closed()) {
                    LOG(WARNING) << "AI gateway session=" << SanitizeForLog(session_id_)
                                 << " failed handling invalid opcode detail='"
                                 << SanitizeForLog(status.message()) << "'";
                }
            }
            MaybeFinish();
            return;
        }

        const std::string payload = beast::buffers_to_string(read_buffer_.data());
        read_buffer_.consume(read_buffer_.size());
        const absl::Status status = adapter_ == nullptr
                                        ? failed_precondition("websocket session is not ready")
                                        : adapter_->HandleIncomingTextFrame(payload);
        if (!status.ok() && adapter_ != nullptr && adapter_->is_closed()) {
            MaybeFinish();
            return;
        }

        StartRead();
    }

    void DoWriteNext() {
        if (transport_closed_ || pending_writes_.empty()) {
            MaybeFinish();
            return;
        }

        write_in_progress_ = true;
        websocket_.text(true);
        auto self = shared_from_this();
        websocket_.async_write(
            asio::buffer(pending_writes_.front().data(), pending_writes_.front().size()),
            [self](const boost::system::error_code& error, const std::size_t bytes_written) {
                self->OnWrite(error, bytes_written);
            });
    }

    void OnWrite(const boost::system::error_code& error, std::size_t bytes_written) {
        write_in_progress_ = false;

        if (error) {
            if (adapter_ != nullptr) {
                adapter_->HandleSendFailure(format_error("websocket write failed", error));
            }
            pending_writes_.clear();
            DoForceCloseTransport();
            return;
        }
        if (pending_writes_.empty()) {
            LOG(ERROR) << "AI gateway session=" << SanitizeForLog(session_id_)
                       << " write completion arrived without a pending frame";
            MaybeFinish();
            return;
        }
        if (bytes_written != pending_writes_.front().size()) {
            if (adapter_ != nullptr) {
                adapter_->HandleSendFailure("websocket write sent a partial text frame");
            }
            pending_writes_.clear();
            DoForceCloseTransport();
            return;
        }

        pending_writes_.pop_front();
        if (!pending_writes_.empty()) {
            DoWriteNext();
            return;
        }

        if (close_after_writes_) {
            BeginGracefulCloseHandshake();
            return;
        }
        MaybeFinish();
    }

    void RequestGracefulClose() {
        close_after_writes_ = true;
        if (!write_in_progress_ && pending_writes_.empty()) {
            BeginGracefulCloseHandshake();
            return;
        }
        VLOG(1) << "AI gateway session=" << SanitizeForLog(session_id_)
                << " deferring graceful close until pending writes flush pending_writes="
                << pending_writes_.size()
                << " write_in_progress=" << (write_in_progress_ ? "true" : "false");
    }

    void BeginGracefulCloseHandshake() {
        if (transport_closed_ || close_handshake_in_progress_) {
            MaybeFinish();
            return;
        }

        if (!websocket_established_) {
            DoCloseTransport();
            return;
        }

        close_handshake_in_progress_ = true;
        const websocket::close_code close_code = server_shutdown_requested_.load()
                                                     ? websocket::close_code::going_away
                                                     : websocket::close_code::normal;
        auto self = shared_from_this();
        websocket_.async_close(
            close_code, [self](const boost::system::error_code& error) { self->OnClose(error); });
    }

    void OnClose(const boost::system::error_code& error) {
        close_handshake_in_progress_ = false;
        if (transport_closed_) {
            MaybeFinish();
            return;
        }
        if (!error || error == websocket::error::closed) {
            transport_closed_ = true;
            MaybeFinish();
            return;
        }
        if (transport_force_closed_.load() || error == asio::error::operation_aborted) {
            MaybeFinish();
            return;
        }

        VLOG(1) << "AI gateway websocket close handshake failed detail='"
                << SanitizeForLog(error.message()) << "'";
        DoCloseTransport();
    }

    void DoForceCloseTransport() {
        CancelShutdownForceCloseTimer();
        transport_force_closed_.store(true);
        VLOG(1) << "AI gateway session=" << session_id_
                << " force closing transport pending_writes=" << pending_writes_.size()
                << " read_in_progress=" << (read_in_progress_ ? "true" : "false")
                << " write_in_progress=" << (write_in_progress_ ? "true" : "false");
        DoCloseTransport();
    }

    void DoCloseTransport() {
        if (transport_closed_) {
            MaybeFinish();
            return;
        }

        CancelShutdownForceCloseTimer();
        auto& socket = websocket_.next_layer();
        transport_closed_ = true;
        boost::system::error_code error;
        const auto shutdown_result = socket.shutdown(tcp::socket::shutdown_both, error);
        static_cast<void>(shutdown_result);
        error.clear();
        const auto close_result = socket.close(error);
        static_cast<void>(close_result);
        if (error && error != websocket::error::closed) {
            VLOG(1) << "AI gateway close failed detail='" << SanitizeForLog(error.message()) << "'";
        }
        MaybeFinish();
    }

    void MaybeFinish() {
        if (finished_.load() || accept_in_progress_ || read_in_progress_ || write_in_progress_ ||
            close_handshake_in_progress_ || !transport_closed_) {
            return;
        }
        FinishSession();
    }

    void FinishSession() {
        if (finished_.exchange(true)) {
            return;
        }
        CancelShutdownForceCloseTimer();
        closed_.store(true);
        std::lock_guard<std::mutex> lock(state_mutex_);
        finished_cv_.notify_all();
    }

    void ArmShutdownForceCloseTimer() {
        if (shutdown_timer_armed_ ||
            shutdown_write_grace_period_ <= std::chrono::milliseconds::zero()) {
            return;
        }
        shutdown_timer_armed_ = true;
        shutdown_timer_.expires_after(shutdown_write_grace_period_);
        auto self = shared_from_this();
        shutdown_timer_.async_wait([self](const boost::system::error_code& error) {
            self->OnShutdownForceCloseTimer(error);
        });
    }

    void CancelShutdownForceCloseTimer() {
        if (!shutdown_timer_armed_) {
            return;
        }
        shutdown_timer_armed_ = false;
        shutdown_timer_.cancel();
    }

    void OnShutdownForceCloseTimer(const boost::system::error_code& error) {
        if (error == asio::error::operation_aborted) {
            return;
        }
        shutdown_timer_armed_ = false;
        if (transport_closed_ || finished_.load() || !server_shutdown_requested_.load()) {
            return;
        }
        LOG(WARNING) << "AI gateway session=" << SanitizeForLog(session_id_)
                     << " forcing transport close after shutdown grace period pending_writes="
                     << pending_writes_.size();
        DoForceCloseTransport();
    }

    std::mutex state_mutex_;
    std::condition_variable finished_cv_;
    websocket::stream<tcp::socket> websocket_;
    beast::flat_buffer read_buffer_{ kMaxInboundWebSocketMessageBytes };
    GatewaySessionRegistry& registry_;
    GatewayWebSocketSessionFactory& factory_;
    std::unique_ptr<GatewayWebSocketSessionAdapter> adapter_;
    std::deque<std::string> pending_writes_;
    std::string session_id_;
    std::string remote_endpoint_;
    asio::steady_timer shutdown_timer_;
    std::chrono::milliseconds shutdown_write_grace_period_;
    std::atomic<bool> closed_{ false };
    std::atomic<bool> finished_{ false };
    std::atomic<bool> transport_force_closed_{ false };
    std::atomic<bool> server_shutdown_requested_{ false };
    bool accept_in_progress_ = false;
    bool read_in_progress_ = false;
    bool write_in_progress_ = false;
    bool close_after_writes_ = false;
    bool close_handshake_in_progress_ = false;
    bool transport_closed_ = false;
    bool websocket_established_ = false;
    bool shutdown_timer_armed_ = false;
};

} // namespace

class GatewaySessionRegistry::Impl {
  public:
    explicit Impl(GatewayApplicationEventSink* application_sink)
        : application_sink_(application_sink) {}

    void RegisterSession(const std::shared_ptr<GatewayLiveSession>& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[session->session_id()] = session;
    }

    [[nodiscard]] std::shared_ptr<GatewayLiveSession>
    FindSession(std::string_view session_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

    [[nodiscard]] std::size_t SessionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

    void ForwardSessionStarted(const SessionStartedEvent& event) {
        if (application_sink_ != nullptr) {
            application_sink_->OnSessionStarted(event);
        }
    }

    void ForwardAccepted(const TurnAcceptedEvent& event) {
        if (application_sink_ != nullptr) {
            application_sink_->OnTurnAccepted(event);
        }
    }

    void ForwardCancelRequested(const TurnCancelRequestedEvent& event) {
        if (application_sink_ != nullptr) {
            application_sink_->OnTurnCancelRequested(event);
        }
    }

    void ForwardSessionClosed(const SessionClosedEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(event.session_id);
        }
        if (application_sink_ != nullptr) {
            application_sink_->OnSessionClosed(event);
        }
    }

  private:
    GatewayApplicationEventSink* application_sink_ = nullptr;
    mutable std::mutex mutex_;
    absl::flat_hash_map<std::string, std::weak_ptr<GatewayLiveSession>> sessions_;
};

GatewaySessionRegistry::GatewaySessionRegistry(GatewayApplicationEventSink* application_sink)
    : impl_(std::make_unique<Impl>(application_sink)) {}

GatewaySessionRegistry::~GatewaySessionRegistry() = default;

void GatewaySessionRegistry::RegisterSession(const std::shared_ptr<GatewayLiveSession>& session) {
    impl_->RegisterSession(session);
}

std::shared_ptr<GatewayLiveSession>
GatewaySessionRegistry::FindSession(std::string_view session_id) const {
    return impl_->FindSession(session_id);
}

std::size_t GatewaySessionRegistry::SessionCount() const {
    return impl_->SessionCount();
}

void GatewaySessionRegistry::OnSessionStarted(const SessionStartedEvent& event) {
    impl_->ForwardSessionStarted(event);
}

void GatewaySessionRegistry::OnTurnAccepted(const TurnAcceptedEvent& event) {
    impl_->ForwardAccepted(event);
}

void GatewaySessionRegistry::OnTurnCancelRequested(const TurnCancelRequestedEvent& event) {
    impl_->ForwardCancelRequested(event);
}

void GatewaySessionRegistry::OnSessionClosed(const SessionClosedEvent& event) {
    impl_->ForwardSessionClosed(event);
}

class GatewayServer::Impl {
  public:
    Impl(GatewayServerConfig config, GatewayApplicationEventSink* application_sink,
         std::unique_ptr<SessionIdGenerator> session_id_generator)
        : config_(std::move(config)), io_context_(1), session_registry_(application_sink),
          session_factory_(std::move(session_id_generator), config_.telemetry_sink),
          acceptor_(io_context_), application_sink_(application_sink) {}

    ~Impl() {
        Stop();
    }

    [[nodiscard]] absl::Status Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return failed_precondition("gateway server is already running");
        }

        io_context_.restart();
        work_guard_.emplace(asio::make_work_guard(io_context_));
        io_thread_ = std::thread([this] { io_context_.run(); });

        boost::system::error_code error;
        const asio::ip::address address = asio::ip::make_address(config_.bind_host, error);
        if (error) {
            StopIoThread();
            return absl::InvalidArgumentError(format_error("invalid bind_host", error));
        }

        const tcp::endpoint endpoint(address, config_.port);
        const tcp protocol = endpoint.protocol();
        const auto open_result = acceptor_.open(protocol, error);
        static_cast<void>(open_result);
        if (error) {
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor open failed", error));
        }

#if !defined(_WIN32)
        // Windows SO_REUSEADDR permits multiple binders on the same port, so keep the
        // listener exclusive there to avoid local port hijacking.
        const asio::socket_base::reuse_address reuse_address(true);
        const auto set_option_result = acceptor_.set_option(reuse_address, error);
        static_cast<void>(set_option_result);
        if (error) {
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor reuse_address failed", error));
        }
#endif

        const auto bind_result = acceptor_.bind(endpoint, error);
        static_cast<void>(bind_result);
        if (error) {
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor bind failed", error));
        }

        const auto listen_result = acceptor_.listen(config_.listen_backlog, error);
        static_cast<void>(listen_result);
        if (error) {
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor listen failed", error));
        }

        const auto non_blocking_result = acceptor_.non_blocking(true, error);
        static_cast<void>(non_blocking_result);
        if (error) {
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor non_blocking failed", error));
        }

        stop_requested_.store(false);
        running_ = true;
        const tcp::endpoint local_endpoint = acceptor_.local_endpoint(error);
        if (error) {
            boost::system::error_code close_error;
            const auto close_result = acceptor_.close(close_error);
            static_cast<void>(close_result);
            running_ = false;
            StopIoThread();
            return absl::UnavailableError(format_error("acceptor local_endpoint failed", error));
        }
        bound_port_ = local_endpoint.port();
        accept_thread_ = std::thread([this] { AcceptLoop(); });
        reap_thread_ = std::thread([this] { ReapLoop(); });
        LOG(INFO) << "AI gateway server listening bind_host=" << config_.bind_host
                  << " port=" << bound_port_;
        return absl::OkStatus();
    }

    void Stop() {
        std::vector<std::shared_ptr<LiveGatewaySession>> sessions_to_stop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            stop_requested_.store(true);
            LOG(INFO) << "AI gateway server stopping active_sessions=" << sessions_.size();
            boost::system::error_code error;
            const auto cancel_result = acceptor_.cancel(error);
            static_cast<void>(cancel_result);
            const auto close_result = acceptor_.close(error);
            static_cast<void>(close_result);
            sessions_to_stop = sessions_;
        }
        if (application_sink_ != nullptr) {
            LOG(INFO) << "AI gateway server finalizing accepted turns before stop"
                      << " active_sessions=" << sessions_to_stop.size();
            application_sink_->OnServerStopping(session_registry_);
        }
        reap_cv_.notify_all();

        for (const auto& session : sessions_to_stop) {
            if (session != nullptr) {
                session->RequestStop();
            }
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (reap_thread_.joinable()) {
            reap_thread_.join();
        }

        std::vector<std::shared_ptr<LiveGatewaySession>> remaining_sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining_sessions.swap(sessions_);
        }
        for (const auto& session : remaining_sessions) {
            if (session != nullptr) {
                session->RequestStop();
                session->Join();
            }
        }

        StopIoThread();

        std::lock_guard<std::mutex> lock(mutex_);
        bound_port_ = 0;
        running_ = false;
        LOG(INFO) << "AI gateway server stopped";
    }

    [[nodiscard]] bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    [[nodiscard]] std::uint16_t bound_port() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bound_port_;
    }

    [[nodiscard]] GatewaySessionRegistry& session_registry() {
        return session_registry_;
    }

    [[nodiscard]] const GatewaySessionRegistry& session_registry() const {
        return session_registry_;
    }

  private:
    void StopIoThread() {
        work_guard_.reset();
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    void AcceptLoop() {
        VLOG(1) << "AI gateway accept loop started";
        while (!stop_requested_.load()) {
            boost::system::error_code error;
            tcp::socket socket(io_context_);
            const auto accept_result = acceptor_.accept(socket, error);
            static_cast<void>(accept_result);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    constexpr std::chrono::milliseconds kAcceptRetryDelay(25);
                    std::this_thread::sleep_for(kAcceptRetryDelay);
                    continue;
                }
                if (stop_requested_.load()) {
                    VLOG(1) << "AI gateway accept loop exiting for server stop";
                    break;
                }
                LOG(WARNING) << "AI gateway accept loop error detail='"
                             << SanitizeForLog(error.message()) << "'";
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }

            boost::system::error_code remote_error;
            const tcp::endpoint remote_endpoint = socket.remote_endpoint(remote_error);
            const std::string remote_endpoint_label =
                remote_error ? std::string("<unknown>") : format_endpoint(remote_endpoint);
            VLOG(1) << "AI gateway accepted TCP connection remote=" << remote_endpoint_label;

            auto session = std::make_shared<LiveGatewaySession>(
                std::move(socket), session_registry_, session_factory_, remote_endpoint_label,
                config_.shutdown_write_grace_period);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.push_back(session);
            }
            session->Start();
        }
        VLOG(1) << "AI gateway accept loop stopped";
    }

    void ReapLoop() {
        VLOG(1) << "AI gateway session reaper started";
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_requested_.load()) {
            constexpr std::chrono::milliseconds kReapWaitInterval(250);
            reap_cv_.wait_for(lock, kReapWaitInterval);
            lock.unlock();
            ReapClosedSessions();
            lock.lock();
        }
        lock.unlock();
        ReapClosedSessions();
        VLOG(1) << "AI gateway session reaper stopped";
    }

    void ReapClosedSessions() {
        std::vector<std::shared_ptr<LiveGatewaySession>> closed_sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto next =
                std::remove_if(sessions_.begin(), sessions_.end(),
                               [](const std::shared_ptr<LiveGatewaySession>& session) {
                                   return session != nullptr && session->is_closed();
                               });
            std::move(next, sessions_.end(), std::back_inserter(closed_sessions));
            sessions_.erase(next, sessions_.end());
        }

        for (const auto& session : closed_sessions) {
            if (session != nullptr) {
                session->Join();
            }
        }
    }

    GatewayServerConfig config_;
    asio::io_context io_context_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    GatewaySessionRegistry session_registry_;
    GatewayWebSocketSessionFactory session_factory_;
    tcp::acceptor acceptor_;
    GatewayApplicationEventSink* application_sink_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable reap_cv_;
    std::thread io_thread_;
    std::thread accept_thread_;
    std::thread reap_thread_;
    std::vector<std::shared_ptr<LiveGatewaySession>> sessions_;
    std::atomic<bool> stop_requested_{ false };
    bool running_ = false;
    std::uint16_t bound_port_ = 0;
};

GatewayServer::GatewayServer(GatewayServerConfig config,
                             GatewayApplicationEventSink* application_sink,
                             std::unique_ptr<SessionIdGenerator> session_id_generator)
    : impl_(std::make_unique<Impl>(std::move(config), application_sink,
                                   std::move(session_id_generator))) {}

GatewayServer::~GatewayServer() = default;

absl::Status GatewayServer::Start() {
    return impl_->Start();
}

void GatewayServer::Stop() {
    impl_->Stop();
}

bool GatewayServer::is_running() const {
    return impl_->is_running();
}

std::uint16_t GatewayServer::bound_port() const {
    return impl_->bound_port();
}

GatewaySessionRegistry& GatewayServer::session_registry() {
    return impl_->session_registry();
}

const GatewaySessionRegistry& GatewayServer::session_registry() const {
    return impl_->session_registry();
}

} // namespace isla::server::ai_gateway
