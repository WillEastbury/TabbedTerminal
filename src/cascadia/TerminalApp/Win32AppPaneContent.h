// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"

namespace winrt::TerminalApp::implementation
{
    class Win32AppPaneContent : public winrt::implements<Win32AppPaneContent, IPaneContent>, public BasicPaneEvents
    {
    public:
        Win32AppPaneContent(const winrt::hstring& executable, const winrt::hstring& title, const winrt::hstring& args = L"");

        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();

        winrt::Windows::Foundation::Size MinimumSize();
        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(const BuildStartupKind kind) const;

        winrt::hstring Title() { return _title; }
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return true; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept;
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

    private:
        winrt::Windows::UI::Xaml::Controls::Grid _grid{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _statusText{ nullptr };
        winrt::hstring _executable;
        winrt::hstring _args;
        winrt::hstring _title;
        HWND _embeddedHwnd{ nullptr };
        HWND _hostHwnd{ nullptr };
        RECT _lastRect{ 0, 0, 0, 0 };
        DWORD _processId{ 0 };
        HANDLE _processHandle{ nullptr };
        bool _closed{ false };
        bool _embedded{ false };
        bool _launched{ false };

        void _BuildUI();
        void _LaunchAndEmbed();
        void _RepositionEmbeddedWindow();
        void _ShowEmbedded(bool show);
        static HWND _FindWindowByProcess(HANDLE process, DWORD pid, int maxWaitMs, bool& processExited);
        static HWND _FindTopLevelWindow();
    };
}
