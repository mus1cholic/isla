#include "win32_layered_overlay.hpp"

#include "absl/log/log.h"
#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace isla::client {

#if defined(_WIN32)

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
        LOG(ERROR) << "Win32Overlay: configure called with null SDL window";
        return false;
    }

    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        LOG(ERROR) << "Win32Overlay: SDL window does not expose a Win32 HWND";
        return false;
    }

    const LONG_PTR existing_ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR overlay_ex_style =
        (existing_ex_style | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_APPWINDOW) &
        ~WS_EX_TOOLWINDOW;
    if (SetWindowLongPtr(hwnd, GWL_EXSTYLE, overlay_ex_style) == 0 && GetLastError() != 0) {
        LOG(ERROR) << "Win32Overlay: failed to apply extended window styles";
        return false;
    }
    // A non-owned top-level window with APPWINDOW is eligible for taskbar/Alt-Tab.
    SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, 0);

    // Force full-window transparency; this is the most reliable baseline for layered overlays.
    if (!SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA)) {
        LOG(ERROR) << "Win32Overlay: SetLayeredWindowAttributes(LWA_ALPHA,0) failed";
        return false;
    }
    if (!SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                          SWP_SHOWWINDOW)) {
        LOG(ERROR) << "Win32Overlay: SetWindowPos failed";
        return false;
    }
    ShowWindow(hwnd, SW_SHOWNA);

    if (g_overlay_original_wndproc == nullptr) {
        g_overlay_original_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&overlay_window_proc)));
        if (g_overlay_original_wndproc == nullptr && GetLastError() != 0) {
            LOG(ERROR) << "Win32Overlay: failed to install overlay window procedure";
            return false;
        }
    }

    LOG(INFO) << "Win32Overlay: layered transparent overlay configured";
    return true;
}

bool refresh_win32_layered_overlay_surface(SDL_Window* window) {
    if (window == nullptr) {
        return false;
    }
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        return false;
    }
    return SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA) != FALSE;
}

#else

bool configure_win32_layered_overlay(SDL_Window* window) {
    (void)window;
    LOG(INFO) << "Win32Overlay: skipped (non-Windows build)";
    return false;
}

bool refresh_win32_layered_overlay_surface(SDL_Window* window) {
    (void)window;
    return false;
}

#endif

} // namespace isla::client
