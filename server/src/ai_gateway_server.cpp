#include "ai_gateway_server.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>

#include "absl/log/log.h"
#include "ai_gateway_logging_utils.hpp"

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
                       GatewayWebSocketSessionFactory& factory, std::string remote_endpoint)
        : websocket_(std::move(socket)), registry_(registry), factory_(factory),
          remote_endpoint_(std::move(remote_endpoint)) {}

    ~LiveGatewaySession() override {
        RequestStop();
        Join();
    }

    void Start() {
        thread_ = std::thread([self = shared_from_this()] { self->Run(); });
    }

    void Join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void RequestStop() {
        ForceCloseTransport();
    }

    [[nodiscard]] const std::string& session_id() const override {
        return session_id_;
    }

    [[nodiscard]] bool is_closed() const override {
        return closed_.load();
    }

    [[nodiscard]] absl::Status SendTextFrame(std::string_view frame) override {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        if (closed_.load()) {
            return failed_precondition("websocket session is closed");
        }

        boost::system::error_code error;
        websocket_.text(true);
        const std::size_t bytes_written =
            websocket_.write(asio::buffer(frame.data(), frame.size()), error);
        if (error) {
            return absl::UnavailableError(format_error("websocket write failed", error));
        }
        if (bytes_written != frame.size()) {
            return absl::UnavailableError("websocket write sent a partial text frame");
        }
        return absl::OkStatus();
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        if (closed_.load()) {
            return;
        }

        auto& socket = websocket_.next_layer();
        boost::system::error_code error;
        const auto shutdown_result = socket.shutdown(tcp::socket::shutdown_both, error);
        static_cast<void>(shutdown_result);
        error.clear();
        const auto close_result = socket.close(error);
        static_cast<void>(close_result);
        if (error && error != websocket::error::closed) {
            VLOG(1) << "AI gateway close failed detail='" << SanitizeForLog(error.message()) << "'";
        }
        closed_.store(true);
    }

  private:
    void ForceCloseTransport() {
        if (transport_force_closed_.exchange(true)) {
            return;
        }

        auto& socket = websocket_.next_layer();
        boost::system::error_code error;
        const auto shutdown_result = socket.shutdown(tcp::socket::shutdown_both, error);
        static_cast<void>(shutdown_result);
        error.clear();
        const auto close_result = socket.close(error);
        static_cast<void>(close_result);
    }

    void Run() {
        {
            std::lock_guard<std::mutex> lock(websocket_mutex_);
            boost::system::error_code error;
            websocket_.set_option(
                websocket::stream_base::timeout::suggested(beast::role_type::server));
            websocket_.accept(error);
            if (error) {
                LOG(WARNING) << "AI gateway rejected websocket handshake remote="
                             << remote_endpoint_ << " detail='" << SanitizeForLog(error.message())
                             << "'";
                auto& socket = websocket_.next_layer();
                boost::system::error_code close_error;
                const auto close_result = socket.close(close_error);
                static_cast<void>(close_result);
                closed_.store(true);
                return;
            }
        }

        adapter_ = factory_.CreateSession(*this, &registry_);
        if (adapter_ == nullptr) {
            LOG(ERROR) << "AI gateway could not create a websocket session adapter";
            Close();
            return;
        }

        session_id_ = adapter_->session_id();
        registry_.RegisterSession(shared_from_this());
        VLOG(1) << "AI gateway accepted websocket transport remote=" << remote_endpoint_
                << " session=" << session_id_;

        while (!adapter_->is_closed()) {
            beast::flat_buffer buffer;
            boost::system::error_code error;

            {
                std::lock_guard<std::mutex> lock(websocket_mutex_);
                const std::size_t bytes_read = websocket_.read(buffer, error);
                static_cast<void>(bytes_read);
            }

            if (error) {
                if (transport_force_closed_.load()) {
                    VLOG(1) << "AI gateway session=" << session_id_
                            << " transport closed by server stop remote=" << remote_endpoint_;
                    adapter_->HandleServerShutdown();
                } else if (error == websocket::error::closed) {
                    adapter_->HandleTransportClosed();
                } else {
                    const absl::Status status = adapter_->HandleTransportError(error.message());
                    if (!status.ok() && !adapter_->is_closed()) {
                        LOG(WARNING) << "AI gateway session=" << session_id_
                                     << " failed handling transport error detail='"
                                     << SanitizeForLog(status.message()) << "'";
                    }
                }
                break;
            }

            if (!websocket_.got_text()) {
                const absl::Status status =
                    adapter_->HandleTransportError("unsupported websocket opcode");
                if (!status.ok() && !adapter_->is_closed()) {
                    LOG(WARNING) << "AI gateway session=" << session_id_
                                 << " failed handling invalid opcode detail='"
                                 << SanitizeForLog(status.message()) << "'";
                }
                break;
            }

            const std::string payload = beast::buffers_to_string(buffer.data());
            const absl::Status status = adapter_->HandleIncomingTextFrame(payload);
            if (!status.ok() && adapter_->is_closed()) {
                break;
            }
        }

        closed_.store(true);
        std::lock_guard<std::mutex> lock(websocket_mutex_);
        auto& socket = websocket_.next_layer();
        boost::system::error_code error;
        const auto close_result = socket.close(error);
        static_cast<void>(close_result);
    }

    mutable std::mutex websocket_mutex_;
    websocket::stream<tcp::socket> websocket_;
    GatewaySessionRegistry& registry_;
    GatewayWebSocketSessionFactory& factory_;
    std::unique_ptr<GatewayWebSocketSessionAdapter> adapter_;
    std::thread thread_;
    std::string session_id_;
    std::string remote_endpoint_;
    std::atomic<bool> closed_{ false };
    std::atomic<bool> transport_force_closed_{ false };
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
        const auto it = sessions_.find(std::string(session_id));
        if (it == sessions_.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

    [[nodiscard]] std::size_t SessionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
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
    std::unordered_map<std::string, std::weak_ptr<GatewayLiveSession>> sessions_;
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
          session_factory_(std::move(session_id_generator)), acceptor_(io_context_) {}

    ~Impl() {
        Stop();
    }

    [[nodiscard]] absl::Status Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return failed_precondition("gateway server is already running");
        }

        boost::system::error_code error;
        const asio::ip::address address = asio::ip::make_address(config_.bind_host, error);
        if (error) {
            return absl::InvalidArgumentError(format_error("invalid bind_host", error));
        }

        const tcp::endpoint endpoint(address, config_.port);
        const tcp protocol = endpoint.protocol();
        const auto open_result = acceptor_.open(protocol, error);
        static_cast<void>(open_result);
        if (error) {
            return absl::UnavailableError(format_error("acceptor open failed", error));
        }

        const asio::socket_base::reuse_address reuse_address(true);
        const auto set_option_result = acceptor_.set_option(reuse_address, error);
        static_cast<void>(set_option_result);
        if (error) {
            return absl::UnavailableError(format_error("acceptor reuse_address failed", error));
        }

        const auto bind_result = acceptor_.bind(endpoint, error);
        static_cast<void>(bind_result);
        if (error) {
            return absl::UnavailableError(format_error("acceptor bind failed", error));
        }

        const auto listen_result = acceptor_.listen(config_.listen_backlog, error);
        static_cast<void>(listen_result);
        if (error) {
            return absl::UnavailableError(format_error("acceptor listen failed", error));
        }

        stop_requested_.store(false);
        running_ = true;
        const tcp::endpoint local_endpoint = acceptor_.local_endpoint(error);
        if (error) {
            boost::system::error_code close_error;
            const auto close_result = acceptor_.close(close_error);
            static_cast<void>(close_result);
            running_ = false;
            return absl::UnavailableError(format_error("acceptor local_endpoint failed", error));
        }
        bound_port_ = local_endpoint.port();
        accept_thread_ = std::thread([this] { AcceptLoop(); });
        LOG(INFO) << "AI gateway server listening bind_host=" << config_.bind_host
                  << " port=" << bound_port_;
        return absl::OkStatus();
    }

    void Stop() {
        std::vector<std::shared_ptr<LiveGatewaySession>> sessions;
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
            sessions = sessions_;
        }

        for (const auto& session : sessions) {
            if (session != nullptr) {
                session->RequestStop();
            }
        }
        for (const auto& session : sessions) {
            if (session != nullptr) {
                session->Join();
            }
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
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
    void AcceptLoop() {
        VLOG(1) << "AI gateway accept loop started";
        while (!stop_requested_.load()) {
            boost::system::error_code error;
            tcp::socket socket(io_context_);
            const auto accept_result = acceptor_.accept(socket, error);
            static_cast<void>(accept_result);
            if (error) {
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
                std::move(socket), session_registry_, session_factory_, remote_endpoint_label);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.push_back(session);
            }
            session->Start();
        }
        VLOG(1) << "AI gateway accept loop stopped";
    }

    GatewayServerConfig config_;
    asio::io_context io_context_;
    GatewaySessionRegistry session_registry_;
    GatewayWebSocketSessionFactory session_factory_;
    tcp::acceptor acceptor_;
    mutable std::mutex mutex_;
    std::thread accept_thread_;
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
