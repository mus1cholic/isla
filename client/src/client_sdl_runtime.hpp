#pragma once

#include <cstdint>

union SDL_Event;
struct SDL_Renderer;
struct SDL_Window;

namespace isla::client {

class ISdlRuntime {
  public:
    virtual ~ISdlRuntime() = default;

    [[nodiscard]] virtual std::uint64_t get_ticks_ns() const = 0;
    [[nodiscard]] virtual bool init_video() const = 0;
    virtual void quit() const = 0;
    [[nodiscard]] virtual bool has_primary_display() const = 0;
    [[nodiscard]] virtual SDL_Window* create_window(const char* title, int width, int height,
                                                    std::uint64_t flags) const = 0;
    [[nodiscard]] virtual SDL_Renderer* create_renderer(SDL_Window* window) const = 0;
    virtual void destroy_renderer(SDL_Renderer* renderer) const = 0;
    virtual void destroy_window(SDL_Window* window) const = 0;
    virtual void maximize_window(SDL_Window* window) const = 0;
    [[nodiscard]] virtual bool poll_event(SDL_Event* event) const = 0;
    [[nodiscard]] virtual const bool* get_keyboard_state(int* key_count) const = 0;
    [[nodiscard]] virtual bool get_window_size_in_pixels(SDL_Window* window, int* width,
                                                         int* height) const = 0;
    [[nodiscard]] virtual bool get_window_size(SDL_Window* window, int* width,
                                               int* height) const = 0;
    virtual void set_window_bordered(SDL_Window* window, bool bordered) const = 0;
    [[nodiscard]] virtual bool set_window_relative_mouse_mode(SDL_Window* window,
                                                              bool enabled) const = 0;
};

class SdlRuntime final : public ISdlRuntime {
  public:
    [[nodiscard]] std::uint64_t get_ticks_ns() const override;
    [[nodiscard]] bool init_video() const override;
    void quit() const override;
    [[nodiscard]] bool has_primary_display() const override;
    [[nodiscard]] SDL_Window* create_window(const char* title, int width, int height,
                                            std::uint64_t flags) const override;
    [[nodiscard]] SDL_Renderer* create_renderer(SDL_Window* window) const override;
    void destroy_renderer(SDL_Renderer* renderer) const override;
    void destroy_window(SDL_Window* window) const override;
    void maximize_window(SDL_Window* window) const override;
    [[nodiscard]] bool poll_event(SDL_Event* event) const override;
    [[nodiscard]] const bool* get_keyboard_state(int* key_count) const override;
    [[nodiscard]] bool get_window_size_in_pixels(SDL_Window* window, int* width,
                                                 int* height) const override;
    [[nodiscard]] bool get_window_size(SDL_Window* window, int* width, int* height) const override;
    void set_window_bordered(SDL_Window* window, bool bordered) const override;
    [[nodiscard]] bool set_window_relative_mouse_mode(SDL_Window* window,
                                                      bool enabled) const override;
};

[[nodiscard]] const ISdlRuntime& default_sdl_runtime();

} // namespace isla::client

