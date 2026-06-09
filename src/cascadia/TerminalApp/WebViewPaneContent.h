// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"
#include <WebView2.h>
#include <wil/com.h>

namespace winrt::TerminalApp::implementation
{
    // Hosts a real WebView2 (Chromium/Edge) on an HWND parented to the Terminal
    // window. The legacy Windows.UI.Xaml.Controls.WebView (EdgeHTML) is retired
    // on modern Windows and renders blank, so we use the native CoreWebView2 COM
    // API and position the controller over our XAML Grid.
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
        winrt::Windows::UI::Xaml::Controls::TextBlock _statusText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _urlBox{ nullptr };
        winrt::hstring _title;
        winrt::hstring _url;

        wil::com_ptr<ICoreWebView2Controller> _controller;
        wil::com_ptr<ICoreWebView2> _webview;
        HWND _hostHwnd{ nullptr };
        RECT _lastBounds{ 0, 0, 0, 0 };
        bool _closed{ false };
        bool _initStarted{ false };
        bool _ready{ false };

        void _BuildUI();
        void _InitWebView2();
        void _UpdateBounds();
        void _SetUrlText(const winrt::hstring& text);
    };
}
