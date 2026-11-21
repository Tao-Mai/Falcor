#pragma once

namespace Falcor { class Window; }

namespace Mogwai
{
    /** Window placement and temporary input language for an interactive Mogwai session. */
    class WindowSession
    {
    public:
        // Capture before creating Mogwai's window, while the previous application still has focus.
        static void* captureInputLayout();
        WindowSession(Falcor::Window* pWindow, void* previousInputLayout);
        ~WindowSession();
        WindowSession(const WindowSession&) = delete;
        WindowSession& operator=(const WindowSession&) = delete;

    private:
        void* mPreviousInputLayout = nullptr;
        void* mLoadedInputLayout = nullptr;
    };
}
