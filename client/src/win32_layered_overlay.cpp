#include "win32_layered_overlay.hpp"

#include "absl/log/log.h"
#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <bit>
#include <dwmapi.h>
#include <iomanip>
#include <windows.h>

#endif

namespace isla::client {

#if defined(_WIN32)

namespace {

WNDPROC g_overlay_original_wndproc = nullptr;

enum class OverlayCompositionMode {
    Unknown = 0,
    NonLayeredDirectComposition,
    LayeredFallback,
};

OverlayCompositionMode g_overlay_composition_mode = OverlayCompositionMode::Unknown;

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

bool configure_win32_alpha_composited_overlay(SDL_Window* window) {
    g_overlay_composition_mode = OverlayCompositionMode::Unknown;
    if (window == nullptr) {
        LOG(ERROR) << "Win32Overlay: alpha-composited configure called with null SDL window";
        return false;
    }

    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        LOG(ERROR) << "Win32Overlay: SDL window does not expose a Win32 HWND";
        return false;
    }

    BOOL composition_enabled = FALSE;
    const HRESULT composition_hr = DwmIsCompositionEnabled(&composition_enabled);
    LOG(INFO) << "Win32Overlay: DwmIsCompositionEnabled hr=0x" << std::hex << composition_hr
              << std::dec << " enabled=" << (composition_enabled != FALSE ? "true" : "false");
    if (FAILED(composition_hr) || composition_enabled == FALSE) {
        LOG(ERROR) << "Win32Overlay: DWM composition unavailable for alpha-composited overlay";
        return false;
    }

    const LONG_PTR existing_ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    // Preferred DirectComposition path: non-layered style so per-pixel swapchain alpha is honored.
    const LONG_PTR non_layered_overlay_ex_style =
        (existing_ex_style | WS_EX_TRANSPARENT | WS_EX_APPWINDOW) &
        ~(WS_EX_LAYERED | WS_EX_TOOLWINDOW);
    SetLastError(0);
    if (SetWindowLongPtr(hwnd, GWL_EXSTYLE, non_layered_overlay_ex_style) == 0 &&
        GetLastError() != 0) {
        const DWORD non_layered_error = GetLastError();
        LOG(WARNING) << "Win32Overlay: non-layered style apply failed last_error=0x" << std::hex
                     << non_layered_error << std::dec
                     << "; falling back to layered-alpha compatibility style";

        const LONG_PTR layered_overlay_ex_style =
            (existing_ex_style | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_APPWINDOW) &
            ~WS_EX_TOOLWINDOW;
        SetLastError(0);
        if (SetWindowLongPtr(hwnd, GWL_EXSTYLE, layered_overlay_ex_style) == 0 &&
            GetLastError() != 0) {
            const DWORD layered_style_error = GetLastError();
            LOG(ERROR) << "Win32Overlay: layered fallback style apply failed last_error=0x"
                       << std::hex << layered_style_error << std::dec;
            return false;
        }
        if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
            const DWORD layered_error = GetLastError();
            LOG(ERROR) << "Win32Overlay: SetLayeredWindowAttributes(LWA_ALPHA,255) failed "
                       << "last_error=0x" << std::hex << layered_error << std::dec;
            return false;
        }
        g_overlay_composition_mode = OverlayCompositionMode::LayeredFallback;
        LOG(INFO) << "Win32Overlay: using layered-alpha fallback style";
    } else {
        g_overlay_composition_mode = OverlayCompositionMode::NonLayeredDirectComposition;
        LOG(INFO) << "Win32Overlay: using non-layered DirectComposition style";
    }
    // A non-owned top-level window with APPWINDOW is eligible for taskbar/Alt-Tab.
    SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, 0);

    // DirectComposition path should not depend on legacy blur/frame extension APIs.
    LOG(INFO) << "Win32Overlay: skipping DwmEnableBlurBehindWindow/DwmExtendFrameIntoClientArea "
                 "for DirectComposition path";

    if (SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                         SWP_SHOWWINDOW) == 0) {
        LOG(ERROR) << "Win32Overlay: SetWindowPos failed";
        return false;
    }
    ShowWindow(hwnd, SW_SHOWNA);

    if (g_overlay_original_wndproc == nullptr) {
        const LONG_PTR previous_wndproc =
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, std::bit_cast<LONG_PTR>(&overlay_window_proc));
        g_overlay_original_wndproc = std::bit_cast<WNDPROC>(previous_wndproc);
        if (g_overlay_original_wndproc == nullptr && GetLastError() != 0) {
            LOG(ERROR) << "Win32Overlay: failed to install overlay window procedure";
            return false;
        }
    }

    LOG(INFO) << "Win32Overlay: alpha-composited overlay configured";
    return true;
}

bool refresh_win32_alpha_composited_overlay(SDL_Window* window) {
    if (window == nullptr) {
        return false;
    }
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        return false;
    }

    if (g_overlay_composition_mode == OverlayCompositionMode::LayeredFallback) {
        if (SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA) == 0) {
            const DWORD layered_error = GetLastError();
            LOG_EVERY_N_SEC(WARNING, 2.0)
                << "Win32Overlay: layered fallback refresh failed "
                << "last_error=0x" << std::hex << layered_error << std::dec;
            return false;
        }
    }
    return true;
}

#else

bool configure_win32_alpha_composited_overlay(SDL_Window* window) {
    (void)window;
    LOG(INFO) << "Win32Overlay: skipped (non-Windows build)";
    return false;
}

bool refresh_win32_alpha_composited_overlay(SDL_Window* window) {
    (void)window;
    return false;
}

#endif

} // namespace isla::client
