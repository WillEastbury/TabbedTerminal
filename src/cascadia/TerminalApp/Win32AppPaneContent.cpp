// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Win32AppPaneContent.h"
#include "Utils.h"
#include <thread>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    Win32AppPaneContent::Win32AppPaneContent(const winrt::hstring& executable, const winrt::hstring& title, const winrt::hstring& args) :
        _executable(executable),
        _title(title),
        _args(args)
    {
        _BuildUI();
    }

    void Win32AppPaneContent::_BuildUI()
    {
        namespace WUX = winrt::Windows::UI::Xaml;

        _grid = WUX::Controls::Grid();
        _grid.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(255, 20, 20, 20) });

        // Status text (shown while launching, hidden once embedded)
        _statusText = WUX::Controls::TextBlock();
        _statusText.Text(L"Launching " + _title + L"...");
        _statusText.HorizontalAlignment(WUX::HorizontalAlignment::Center);
        _statusText.VerticalAlignment(WUX::VerticalAlignment::Center);
        _statusText.Foreground(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(255, 160, 160, 160) });
        _grid.Children().Append(_statusText);

        // These handlers are owned by `_grid`, which this object owns, so they
        // cannot outlive `this`; capturing the raw pointer is safe and avoids a
        // reference cycle. Do NOT call get_weak()/get_strong() here — _BuildUI
        // runs from the constructor, where the object isn't yet owned by a smart
        // pointer and those calls trip a debug assertion.

        // When the grid is loaded, launch+embed (first time) or re-show (tab switch back)
        _grid.Loaded([this](auto&&, auto&&) {
            if (_closed)
                return;
            if (!_launched)
            {
                _launched = true;
                _LaunchAndEmbed();
            }
            else if (_embedded)
            {
                // Returning to this tab — show and reposition the embedded window
                _ShowEmbedded(true);
                _RepositionEmbeddedWindow();
            }
        });

        // When the grid is unloaded (tab switched away), hide the embedded window
        // so it doesn't float over other tabs (XAML airspace issue)
        _grid.Unloaded([this](auto&&, auto&&) {
            if (_embedded)
            {
                _ShowEmbedded(false);
            }
        });

        // Reposition the embedded window when the grid resizes
        _grid.SizeChanged([this](auto&&, auto&&) {
            if (_embedded)
            {
                _RepositionEmbeddedWindow();
            }
        });

        // Reposition on layout changes (e.g. sidebar resize, window move within monitor)
        _grid.LayoutUpdated([this](auto&&, auto&&) {
            if (_embedded)
            {
                _RepositionEmbeddedWindow();
            }
        });
    }

    HWND Win32AppPaneContent::_FindTopLevelWindow()
    {
        HWND hwnd = GetActiveWindow();
        if (!hwnd)
        {
            hwnd = GetForegroundWindow();
        }
        if (hwnd)
        {
            HWND root = GetAncestor(hwnd, GA_ROOT);
            if (root) return root;
        }
        return hwnd;
    }

    struct EnumWindowsPidData
    {
        DWORD pid;
        HWND result;
    };

    static BOOL CALLBACK EnumWindowsPidProc(HWND hwnd, LPARAM lParam)
    {
        auto* data = reinterpret_cast<EnumWindowsPidData*>(lParam);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == data->pid)
        {
            if (IsWindowVisible(hwnd) && GetParent(hwnd) == nullptr)
            {
                data->result = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }

    HWND Win32AppPaneContent::_FindWindowByProcess(HANDLE process, DWORD pid, int maxWaitMs, bool& processExited)
    {
        processExited = false;
        EnumWindowsPidData data{ pid, nullptr };
        int waited = 0;
        const int interval = 50;

        while (waited < maxWaitMs)
        {
            EnumWindows(EnumWindowsPidProc, reinterpret_cast<LPARAM>(&data));
            if (data.result)
                return data.result;

            // If the launched process has already exited (e.g. calc.exe is a stub
            // that spawns the real UWP app and exits), give up early — there is no
            // window we can embed for this PID.
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
            {
                processExited = true;
                return nullptr;
            }

            Sleep(interval);
            waited += interval;
        }
        return nullptr;
    }

    void Win32AppPaneContent::_LaunchAndEmbed()
    {
        _hostHwnd = _FindTopLevelWindow();
        if (!_hostHwnd)
        {
            _statusText.Text(L"Error: Could not find host window");
            return;
        }

        // Launch the process
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        std::wstring cmdLine = std::wstring(_executable);
        if (!_args.empty())
        {
            cmdLine += L" " + std::wstring(_args);
        }

        BOOL created = CreateProcessW(
            nullptr,
            cmdLine.data(),
            nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);

        if (!created)
        {
            // Try ShellExecuteEx for apps that need shell resolution
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"open";
            sei.lpFile = _executable.c_str();
            sei.lpParameters = _args.empty() ? nullptr : _args.c_str();
            sei.nShow = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&sei))
            {
                _statusText.Text(L"Error: Failed to launch " + _executable);
                return;
            }
            _processHandle = sei.hProcess;
            _processId = GetProcessId(sei.hProcess);
        }
        else
        {
            _processHandle = pi.hProcess;
            _processId = pi.dwProcessId;
            CloseHandle(pi.hThread);
        }

        // Find the window and reparent on a background thread.
        // - Use a WEAK ref for UI-thread dispatches; resolve and bail if the
        //   pane was closed in the meantime (avoids use-after-free).
        // - Duplicate the process handle so the worker owns its own copy; the
        //   member handle may be closed by Close() concurrently.
        auto dispatcher = _grid.Dispatcher();
        auto weakThis = get_weak();
        DWORD pid = _processId;
        HWND hostHwnd = _hostHwnd;

        HANDLE workerProcess = nullptr;
        if (_processHandle)
        {
            DuplicateHandle(GetCurrentProcess(), _processHandle, GetCurrentProcess(),
                &workerProcess, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }

        std::thread([weakThis, dispatcher, pid, workerProcess, hostHwnd]() {
            // A raw std::thread starts with COM/WinRT uninitialized. Calling a
            // projected WinRT API (CoreDispatcher::RunAsync, which builds an
            // agile delegate) from such a thread fails with CO_E_NOTINITIALIZED
            // and throws hresult_error; an exception escaping this thread would
            // call std::terminate()/abort(). Initialize an MTA apartment and
            // guard the whole body so nothing can escape.
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                bool processExited = false;
                HWND targetHwnd = _FindWindowByProcess(workerProcess, pid, 8000, processExited);
                if (workerProcess)
                {
                    CloseHandle(workerProcess);
                }

                if (!targetHwnd)
                {
                    // Report failure back on the UI thread
                    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, processExited]() {
                        try
                        {
                            auto self = weakThis.get();
                            if (!self || self->_closed)
                                return;
                            if (processExited)
                            {
                                self->_statusText.Text(
                                    L"\"" + self->_title + L"\" could not be embedded.\n"
                                    L"It launches a separate process (e.g. a Store app) that\n"
                                    L"cannot be hosted inside a tab.");
                            }
                            else
                            {
                                self->_statusText.Text(L"Timed out waiting for " + self->_title + L" window.");
                            }
                        }
                        catch (...)
                        {
                        }
                    });
                    winrt::uninit_apartment();
                    return;
                }

                Sleep(300); // let the window fully render

                dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [weakThis, targetHwnd, hostHwnd]() {
                    try
                    {
                        auto self = weakThis.get();
                        // Bail if the tab was closed while we were searching
                        if (!self || self->_closed)
                        {
                            return;
                        }

                        self->_embeddedHwnd = targetHwnd;

                        // Remove title bar, borders, make it a child
                        LONG style = GetWindowLong(targetHwnd, GWL_STYLE);
                        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_POPUP);
                        style |= WS_CHILD | WS_VISIBLE;
                        SetWindowLong(targetHwnd, GWL_STYLE, style);

                        LONG exStyle = GetWindowLong(targetHwnd, GWL_EXSTYLE);
                        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_APPWINDOW);
                        exStyle |= WS_EX_TOOLWINDOW;
                        SetWindowLong(targetHwnd, GWL_EXSTYLE, exStyle);

                        // Reparent
                        SetParent(targetHwnd, hostHwnd);

                        self->_embedded = true;

                        // Position
                        self->_RepositionEmbeddedWindow();

                        // Hide status
                        self->_statusText.Visibility(Visibility::Collapsed);

                        // Force redraw
                        SetWindowPos(targetHwnd, nullptr, 0, 0, 0, 0,
                            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
                        ShowWindow(targetHwnd, SW_SHOW);
                    }
                    catch (...)
                    {
                    }
                });
            }
            catch (...)
            {
            }
            winrt::uninit_apartment();
        }).detach();
    }

    void Win32AppPaneContent::_ShowEmbedded(bool show)
    {
        if (_embeddedHwnd)
        {
            ShowWindow(_embeddedHwnd, show ? SW_SHOW : SW_HIDE);
        }
    }

    void Win32AppPaneContent::_RepositionEmbeddedWindow()
    {
        if (!_embeddedHwnd || !_hostHwnd)
            return;

        try
        {
            auto transform = _grid.TransformToVisual(nullptr);
            auto point = transform.TransformPoint(Point(0, 0));
            auto size = _grid.ActualSize();

            float dpiScale = static_cast<float>(GetDpiForWindow(_hostHwnd)) / 96.0f;
            int x = static_cast<int>(point.X * dpiScale);
            int y = static_cast<int>(point.Y * dpiScale);
            int w = static_cast<int>(size.x * dpiScale);
            int h = static_cast<int>(size.y * dpiScale);

            if (w <= 0 || h <= 0)
                return;

            // Skip redundant moves — LayoutUpdated fires very frequently and
            // MoveWindow on every tick would be wasteful and could flicker.
            RECT newRect{ x, y, x + w, y + h };
            if (newRect.left == _lastRect.left && newRect.top == _lastRect.top &&
                newRect.right == _lastRect.right && newRect.bottom == _lastRect.bottom)
            {
                return;
            }
            _lastRect = newRect;

            MoveWindow(_embeddedHwnd, x, y, w, h, TRUE);
        }
        catch (...)
        {
        }
    }

    void Win32AppPaneContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
    }

    FrameworkElement Win32AppPaneContent::GetRoot()
    {
        return _grid;
    }

    Size Win32AppPaneContent::MinimumSize()
    {
        return { 200, 200 };
    }

    void Win32AppPaneContent::Focus(FocusState /*reason*/)
    {
        if (!_embeddedHwnd)
            return;

        // The embedded window belongs to another thread/process. A plain
        // SetFocus across threads is ignored unless the input queues are
        // attached, so temporarily attach to hand it focus.
        DWORD targetThread = GetWindowThreadProcessId(_embeddedHwnd, nullptr);
        DWORD thisThread = GetCurrentThreadId();
        if (targetThread && targetThread != thisThread)
        {
            AttachThreadInput(thisThread, targetThread, TRUE);
            SetFocus(_embeddedHwnd);
            AttachThreadInput(thisThread, targetThread, FALSE);
        }
        else
        {
            SetFocus(_embeddedHwnd);
        }
    }

    void Win32AppPaneContent::Close()
    {
        _closed = true;

        if (_embeddedHwnd)
        {
            // Un-parent and restore a normal window style so the app can close
            // gracefully on its own thread.
            LONG style = GetWindowLong(_embeddedHwnd, GWL_STYLE);
            style &= ~WS_CHILD;
            style |= WS_OVERLAPPEDWINDOW;
            SetWindowLong(_embeddedHwnd, GWL_STYLE, style);
            SetParent(_embeddedHwnd, nullptr);
            PostMessage(_embeddedHwnd, WM_CLOSE, 0, 0);
            _embeddedHwnd = nullptr;
            _embedded = false;
        }

        // Terminate the process we launched. Give a WM_CLOSE-driven graceful
        // exit a brief chance first, then force-terminate to avoid leaks.
        if (_processHandle)
        {
            if (WaitForSingleObject(_processHandle, 500) != WAIT_OBJECT_0)
            {
                TerminateProcess(_processHandle, 0);
            }
            CloseHandle(_processHandle);
            _processHandle = nullptr;
        }
    }

    INewContentArgs Win32AppPaneContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"REPARENT:" + _executable);
    }

    winrt::hstring Win32AppPaneContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE737" };
        return winrt::hstring{ glyph };
    }

    IReference<winrt::Windows::UI::Color> Win32AppPaneContent::TabColor() const noexcept
    {
        return nullptr;
    }

    winrt::Windows::UI::Xaml::Media::Brush Win32AppPaneContent::BackgroundBrush()
    {
        return nullptr;
    }
}
