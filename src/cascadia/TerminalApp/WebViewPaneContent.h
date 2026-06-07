// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"

namespace winrt::TerminalApp::implementation
{
    class WebViewPaneContent : public winrt::implements<WebViewPaneContent, IPaneContent>, public BasicPaneEvents
    {
    public:
        WebViewPaneContent(const winrt::hstring& url, const winrt::hstring& title);

        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);

        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();

        winrt::Windows::Foundation::Size MinimumSize();
        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(const BuildStartupKind kind) const;

        winrt::hstring Title() { return _title; }
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept;
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        void Navigate(const winrt::hstring& url);

    private:
        winrt::Windows::UI::Xaml::Controls::Grid _grid{ nullptr };
        winrt::Windows::UI::Xaml::Controls::WebView _webView{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _urlBox{ nullptr };
        winrt::hstring _title;
        winrt::hstring _url;
        void _BuildUI();
    };
}
