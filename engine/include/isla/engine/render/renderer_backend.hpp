#pragma once

#include "isla/engine/render/render_world.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Renderer;
struct SDL_Window;

namespace isla::client {

inline constexpr int kDefaultRenderWidth = 1280;
inline constexpr int kDefaultRenderHeight = 720;

struct RenderSize {
    int width = kDefaultRenderWidth;
    int height = kDefaultRenderHeight;
};

enum class ChatPanelEntryRole {
    System = 0,
    User,
    Assistant,
};

struct ChatPanelEntry {
    ChatPanelEntryRole role = ChatPanelEntryRole::System;
    std::string text;
};

struct ChatPanelState {
    bool enabled = false;
    bool connected = false;
    bool turn_in_flight = false;
    std::string status_line;
    std::vector<ChatPanelEntry> transcript;
};

class IRendererBackend {
  public:
    IRendererBackend() = default;
    virtual ~IRendererBackend() = default;

    IRendererBackend(const IRendererBackend&) = delete;
    IRendererBackend& operator=(const IRendererBackend&) = delete;
    IRendererBackend(IRendererBackend&&) = delete;
    IRendererBackend& operator=(IRendererBackend&&) = delete;

    virtual bool initialize(SDL_Window* window, SDL_Renderer* renderer, RenderSize size) = 0;
    [[nodiscard]] virtual bool uses_sdl_renderer() const = 0;
    [[nodiscard]] virtual bool has_homogeneous_depth() const = 0;
    virtual void on_resize(RenderSize size) = 0;
    virtual void handle_event(const SDL_Event& event) = 0;
    virtual void render(const RenderWorld& world) = 0;
    virtual void set_debug_overlay_enabled(bool enabled) = 0;
    virtual void set_debug_overlay_lines(std::span<const std::string> lines) = 0;
    virtual void set_chat_panel_state(const ChatPanelState& state) = 0;
    [[nodiscard]] virtual bool wants_keyboard_capture() const = 0;
    virtual std::optional<std::string> take_chat_submit_request() = 0;
    virtual void shutdown() = 0;
};

} // namespace isla::client
