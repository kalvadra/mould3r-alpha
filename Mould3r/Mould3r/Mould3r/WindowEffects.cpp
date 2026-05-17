#include "WindowEffects.h"
#include <wx/window.h>

#ifdef __WXMSW__
#include <windows.h>
#include <dwmapi.h>

// Auto-link the DWM library. Less invasive than editing the .vcxproj
// — MSVC reads the pragma at compile time and adds dwmapi.lib to the
// linker inputs. Other Windows compilers (MinGW, Clang-cl) honour
// this too.
#pragma comment(lib, "dwmapi.lib")

// The DWM corner-preference attribute and its values were added with
// the Windows 11 SDK (10.0.22000). Older SDKs don't define these
// symbols, so we declare them manually with the documented integer
// values from Microsoft's docs. At runtime, on a Win10 machine the
// DwmSetWindowAttribute call returns E_INVALIDARG (the OS doesn't
// recognise attribute 33) and the window stays square — no fallback
// is attempted; the rest of the app still renders correctly.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace
{
    // Mirrors the official DWM_WINDOW_CORNER_PREFERENCE enum. Locally
    // defined so we compile against older SDKs that don't ship it.
    enum LocalCornerPreference
    {
        kCornerDefault = 0,  // OS decides — currently same as Round
        kCornerDoNotRound = 1,
        kCornerRound = 2,  // ~8px radius, matches native Win11 dialogs
        kCornerRoundSmall = 3   // ~4px radius — smaller controls
    };
}
#endif

namespace WindowEffects
{
    void ApplyRoundedCorners(wxWindow* window)
    {
#ifdef __WXMSW__
        if (!window) return;
        const HWND hwnd = static_cast<HWND>(window->GetHWND());
        if (!hwnd) return;

        // sizeof(int) is what the API expects — the enum is passed by
        // address and the size is the size of one enum value. Return
        // value intentionally ignored: a Win10 failure is the expected
        // path on that platform, not an error worth surfacing.
        int pref = kCornerRound;
        ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
            &pref, sizeof(pref));
#else
        (void)window;   // silence unused-parameter warnings on non-MSW builds
#endif
    }
}
