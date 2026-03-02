#include "win32_layered_overlay.hpp"

#include <SDL3/SDL.h>

namespace isla::client {

#if defined(_WIN32)

#include <windows.h>

namespace {

WNDPROC g_overlay_original_wndproc = nullptr;

LRESULT CALLBACK overlay_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }

    if (g_overlay_original_wndproc != nullptr) {
        return CallWindowProc(g_overlay_original_wndproc, hwnd, message, wparam, lparam);
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

} // namespace

bool configure_win32_layered_overlay(SDL_Window* window) {
    if (window == nullptr) {
        return false;
    }

    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        return false;
    }

    const LONG_PTR existing_ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR overlay_ex_style =
        existing_ex_style | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, overlay_ex_style);

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (g_overlay_original_wndproc == nullptr) {
        g_overlay_original_wndproc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                       reinterpret_cast<LONG_PTR>(
                                                           &overlay_window_proc)));
    }

    return true;
}

#else

bool configure_win32_layered_overlay(SDL_Window* window) {
    (void)window;
    return false;
}

#endif

} // namespace isla::client

