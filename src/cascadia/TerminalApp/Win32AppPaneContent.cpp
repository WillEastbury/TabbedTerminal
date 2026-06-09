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

        // When the grid is loaded and has a size, launch and embed
        _grid.Loaded([this](auto&&, auto&&) {
            _LaunchAndEmbed();
        });

        // Reposition the embedded window when the grid resizes
        _grid.SizeChanged([this](auto&&, auto&&) {
            _RepositionEmbeddedWindow();
        });
    }

    HWND Win32AppPaneContent::_FindTopLevelWindow()
    {
        HWND hwnd = GetForegroundWindow();
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

    HWND Win32AppPaneContent::_FindWindowByPid(DWORD pid, int maxWaitMs)
    {
        EnumWindowsPidData data{ pid, nullptr };
        int waited = 0;
        const int interval = 50;

        while (waited < maxWaitMs)
        {
            EnumWindows(EnumWindowsPidProc, reinterpret_cast<LPARAM>(&data));
            if (data.result)
                return data.result;
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
            // Try ShellExecuteEx for apps like calc.exe that redirect
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"open";
            sei.lpFile = _executable.c_str();
            sei.lpParameters = _args.empty() ? nullptr : _args.c_str();
            sei.nShow = SW_HIDE;

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

        // Find the window and reparent (on background thread)
        auto dispatcher = _grid.Dispatcher();
        DWORD pid = _processId;
        HWND hostHwnd = _hostHwnd;

        // Store a raw pointer to self - safe because the thread will dispatch
        // back to the UI thread where the pane is guaranteed alive
        auto* self = this;
        auto gridRef = _grid; // strong ref to keep pane alive

        std::thread([self, dispatcher, pid, hostHwnd, gridRef]() {
            HWND targetHwnd = _FindWindowByPid(pid, 8000);
            if (!targetHwnd)
                return;

            Sleep(300); // let the window fully render

            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [self, targetHwnd, hostHwnd]() {
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

                // Position
                self->_RepositionEmbeddedWindow();

                // Hide status
                self->_statusText.Visibility(Visibility::Collapsed);

                // Force redraw
                SetWindowPos(targetHwnd, nullptr, 0, 0, 0, 0,
                    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
                ShowWindow(targetHwnd, SW_SHOW);
            });
        }).detach();
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

            if (w > 0 && h > 0)
            {
                MoveWindow(_embeddedHwnd, x, y, w, h, TRUE);
            }
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
        if (_embeddedHwnd)
        {
            SetFocus(_embeddedHwnd);
        }
    }

    void Win32AppPaneContent::Close()
    {
        if (_embeddedHwnd)
        {
            LONG style = GetWindowLong(_embeddedHwnd, GWL_STYLE);
            style &= ~WS_CHILD;
            style |= WS_OVERLAPPEDWINDOW;
            SetWindowLong(_embeddedHwnd, GWL_STYLE, style);
            SetParent(_embeddedHwnd, nullptr);
            PostMessage(_embeddedHwnd, WM_CLOSE, 0, 0);
            _embeddedHwnd = nullptr;
        }
        if (_processHandle)
        {
            TerminateProcess(_processHandle, 0);
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
