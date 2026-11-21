#include "Falcor.h"
#include "Core/Window.h"
#include "WindowSession.h"

#if FALCOR_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <vector>
#endif

namespace Mogwai
{
    void* WindowSession::captureInputLayout()
    {
#if FALCOR_WINDOWS
        const auto foreground = GetForegroundWindow();
        const DWORD thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        return GetKeyboardLayout(thread);
#else
        return nullptr;
#endif
    }

    WindowSession::WindowSession(Falcor::Window* pWindow, void* previousInputLayout)
    {
#if FALCOR_WINDOWS
        if (!pWindow) return;

        if (pWindow->getDesc().mode == Falcor::Window::WindowMode::Normal)
        {
            const HWND window = static_cast<HWND>(pWindow->getApiHandle());
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            RECT rect{};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitorInfo) && GetWindowRect(window, &rect))
            {
                // Center the whole decorated window in the usable area, including taskbar clearance.
                const auto& area = monitorInfo.rcWork;
                const LONG width = std::min(rect.right - rect.left, area.right - area.left);
                const LONG height = std::min(rect.bottom - rect.top, area.bottom - area.top);
                const LONG x = area.left + (area.right - area.left - width) / 2;
                const LONG y = area.top + (area.bottom - area.top - height) / 2;
                if (SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE))
                    Falcor::logInfo("Mogwai window centered: position ({}, {}), outer size {}x{}.", x, y, width, height);
            }
        }

        mPreviousInputLayout = previousInputLayout ? previousInputLayout : GetKeyboardLayout(0);
        std::vector<HKL> layouts(std::max(GetKeyboardLayoutList(0, nullptr), 0));
        const int count = layouts.empty() ? 0 : GetKeyboardLayoutList(static_cast<int>(layouts.size()), layouts.data());
        HKL english = nullptr;
        for (int i = 0; i < count; ++i)
        {
            if (LOWORD(reinterpret_cast<ULONG_PTR>(layouts[i])) == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
            {
                english = layouts[i];
                break;
            }
        }
        if (!english)
        {
            english = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
            mLoadedInputLayout = english;
        }
        if (english && LOWORD(reinterpret_cast<ULONG_PTR>(english)) == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
            && ActivateKeyboardLayout(english, 0))
            Falcor::logInfo("Mogwai input layout: ENG (0x{:x}), previous 0x{:x}.",
                reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)), reinterpret_cast<ULONG_PTR>(mPreviousInputLayout));
        else
            Falcor::logWarning("Could not switch Mogwai input layout to ENG (Windows error {}).", GetLastError());
#endif
    }

    WindowSession::~WindowSession()
    {
#if FALCOR_WINDOWS
        // This guard is destroyed before the renderer, so its focused native window still exists.
        if (mPreviousInputLayout)
        {
            if (ActivateKeyboardLayout(static_cast<HKL>(mPreviousInputLayout), 0))
                Falcor::logInfo("Restored input layout: 0x{:x}.", reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
            else
                Falcor::logWarning("Could not restore the previous input layout (Windows error {}).", GetLastError());
        }
        if (mLoadedInputLayout && mLoadedInputLayout != mPreviousInputLayout)
            UnloadKeyboardLayout(static_cast<HKL>(mLoadedInputLayout));
#endif
    }
}
