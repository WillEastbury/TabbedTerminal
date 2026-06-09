// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WebViewPaneContent.h"
#include "Utils.h"
#include <wrl.h>
#include <wil/resource.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Microsoft::WRL;

namespace winrt::TerminalApp::implementation
{
    WebViewPaneContent::WebViewPaneContent(const winrt::hstring& url, const winrt::hstring& title) :
        _url(url),
        _title(title)
    {
        _BuildUI();
    }

    void WebViewPaneContent::_BuildUI()
    {
        namespace WUX = winrt::Windows::UI::Xaml;

        _grid = WUX::Controls::Grid();
        _grid.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(255, 24, 24, 24) });

        WUX::Controls::RowDefinition urlRow;
        urlRow.Height(WUX::GridLengthHelper::Auto());
        WUX::Controls::RowDefinition contentRow;
        contentRow.Height(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
        _grid.RowDefinitions().Append(urlRow);
        _grid.RowDefinitions().Append(contentRow);

        // URL bar: [Back] [Forward] [Refresh] [Home] [URL TextBox] [Go]
        auto urlBar = WUX::Controls::StackPanel();
        urlBar.Orientation(WUX::Controls::Orientation::Horizontal);
        urlBar.Padding(WUX::ThicknessHelper::FromLengths(4, 3, 4, 3));
        urlBar.Spacing(4);
        urlBar.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(255, 30, 30, 30) });

        auto makeNavButton = [](const wchar_t* glyph) {
            auto btn = WUX::Controls::Button();
            WUX::Controls::FontIcon icon;
            icon.FontFamily(WUX::Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            icon.Glyph(glyph);
            icon.FontSize(12);
            btn.Content(icon);
            btn.Padding(WUX::ThicknessHelper::FromLengths(6, 4, 6, 4));
            btn.MinWidth(28);
            btn.MinHeight(28);
            return btn;
        };

        auto backBtn = makeNavButton(L"\xE72B");
        auto fwdBtn = makeNavButton(L"\xE72A");
        auto refreshBtn = makeNavButton(L"\xE72C");
        auto homeBtn = makeNavButton(L"\xE80F");

        _urlBox = WUX::Controls::TextBox();
        _urlBox.Text(_url);
        _urlBox.MinWidth(200);
        _urlBox.VerticalAlignment(WUX::VerticalAlignment::Center);

        auto goBtn = WUX::Controls::Button();
        goBtn.Content(winrt::box_value(L"Go"));
        goBtn.Padding(WUX::ThicknessHelper::FromLengths(10, 4, 10, 4));
        goBtn.MinHeight(28);

        urlBar.Children().Append(backBtn);
        urlBar.Children().Append(fwdBtn);
        urlBar.Children().Append(refreshBtn);
        urlBar.Children().Append(homeBtn);
        urlBar.Children().Append(_urlBox);
        urlBar.Children().Append(goBtn);
        WUX::Controls::Grid::SetRow(urlBar, 0);
        _grid.Children().Append(urlBar);

        // Status text in the content area (shown while initializing)
        _statusText = WUX::Controls::TextBlock();
        _statusText.Text(L"Loading " + _url + L"...");
        _statusText.HorizontalAlignment(WUX::HorizontalAlignment::Center);
        _statusText.VerticalAlignment(WUX::VerticalAlignment::Center);
        _statusText.Foreground(WUX::Media::SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(255, 160, 160, 160) });
        WUX::Controls::Grid::SetRow(_statusText, 1);
        _grid.Children().Append(_statusText);

        // These handlers are owned by XAML elements that this object owns, so
        // they cannot outlive `this`; capturing the raw pointer is safe.
        auto navigateFromBox = [this]() {
            if (!_urlBox)
                return;
            Navigate(_urlBox.Text());
        };

        backBtn.Click([this](auto&&, auto&&) {
            if (_webview) _webview->GoBack();
        });
        fwdBtn.Click([this](auto&&, auto&&) {
            if (_webview) _webview->GoForward();
        });
        refreshBtn.Click([this](auto&&, auto&&) {
            if (_webview) _webview->Reload();
        });
        homeBtn.Click([this](auto&&, auto&&) {
            Navigate(_url);
        });
        goBtn.Click([navigateFromBox](auto&&, auto&&) { navigateFromBox(); });
        _urlBox.KeyDown([navigateFromBox](auto&&, const WUX::Input::KeyRoutedEventArgs& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
                navigateFromBox();
        });

        // Lifecycle: init WebView2 on first load; show/hide on tab switch.
        _grid.Loaded([this](auto&&, auto&&) {
            if (_closed)
                return;
            if (!_initStarted)
            {
                _initStarted = true;
                _InitWebView2();
            }
            else if (_controller)
            {
                _controller->put_IsVisible(TRUE);
                _UpdateBounds();
            }
        });

        _grid.Unloaded([this](auto&&, auto&&) {
            if (_controller)
            {
                _controller->put_IsVisible(FALSE);
            }
        });

        _grid.SizeChanged([this](auto&&, auto&&) {
            _UpdateBounds();
        });
        _grid.LayoutUpdated([this](auto&&, auto&&) {
            _UpdateBounds();
        });
    }

    void WebViewPaneContent::_InitWebView2()
    {
        // Resolve the Terminal's top-level window to parent the WebView2 into.
        _hostHwnd = GetActiveWindow();
        if (!_hostHwnd)
            _hostHwnd = GetForegroundWindow();
        if (!_hostHwnd)
        {
            _statusText.Text(L"Error: no host window for WebView2");
            return;
        }

        // Persistent user-data folder so cookies/logins/state survive restarts.
        std::wstring userDataFolder;
        {
            wchar_t* localAppData = nullptr;
            size_t len = 0;
            if (_wdupenv_s(&localAppData, &len, L"LOCALAPPDATA") == 0 && localAppData)
            {
                userDataFolder = std::wstring(localAppData) + L"\\TabbedTerminal\\WebView2";
                free(localAppData);
            }
        }

        // Keep this object alive across the async init (the tab could close).
        auto strongThis = get_strong();
        const wchar_t* udf = userDataFolder.empty() ? nullptr : userDataFolder.c_str();

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            udf,
            nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [strongThis](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (strongThis->_closed || FAILED(result) || !env)
                    {
                        if (FAILED(result) && !strongThis->_closed)
                        {
                            strongThis->_statusText.Text(L"WebView2 runtime not available.\nInstall the Microsoft Edge WebView2 Runtime.");
                        }
                        return S_OK;
                    }

                    env->CreateCoreWebView2Controller(
                        strongThis->_hostHwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [strongThis](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                                if (strongThis->_closed || FAILED(result2) || !controller)
                                    return S_OK;

                                strongThis->_controller = controller;
                                strongThis->_controller->get_CoreWebView2(strongThis->_webview.put());
                                strongThis->_controller->put_IsVisible(TRUE);
                                strongThis->_ready = true;
                                strongThis->_lastBounds = RECT{ 0, 0, 0, 0 };
                                strongThis->_UpdateBounds();

                                // Update the URL box as the source changes. The
                                // handler is owned by the webview (owned by this
                                // object); guard _closed for safety.
                                if (strongThis->_webview)
                                {
                                    auto* self = strongThis.get();
                                    ::EventRegistrationToken token{};
                                    strongThis->_webview->add_SourceChanged(
                                        Callback<ICoreWebView2SourceChangedEventHandler>(
                                            [self](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                                if (self->_closed)
                                                    return S_OK;
                                                wil::unique_cotaskmem_string uri;
                                                if (SUCCEEDED(sender->get_Source(&uri)) && uri)
                                                {
                                                    self->_SetUrlText(winrt::hstring{ uri.get() });
                                                }
                                                return S_OK;
                                            }).Get(),
                                        &token);

                                    strongThis->_webview->Navigate(strongThis->_url.c_str());
                                }

                                strongThis->_statusText.Visibility(Visibility::Collapsed);
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());

        if (FAILED(hr))
        {
            wchar_t msg[64];
            swprintf_s(msg, L"Failed to start WebView2 (0x%08X)", static_cast<unsigned int>(hr));
            _statusText.Text(msg);
        }
    }

    void WebViewPaneContent::_UpdateBounds()
    {
        if (!_controller || !_hostHwnd)
            return;

        try
        {
            auto transform = _grid.TransformToVisual(nullptr);
            auto origin = transform.TransformPoint(Point(0, 0));
            auto size = _grid.ActualSize();

            // Offset the WebView below the URL bar (row 0).
            float urlBarHeight = 0;
            if (_grid.Children().Size() > 0)
            {
                if (auto fe = _grid.Children().GetAt(0).try_as<FrameworkElement>())
                {
                    urlBarHeight = static_cast<float>(fe.ActualHeight());
                }
            }

            float dpiScale = static_cast<float>(GetDpiForWindow(_hostHwnd)) / 96.0f;
            int x = static_cast<int>(origin.X * dpiScale);
            int y = static_cast<int>((origin.Y + urlBarHeight) * dpiScale);
            int w = static_cast<int>(size.x * dpiScale);
            int h = static_cast<int>((size.y - urlBarHeight) * dpiScale);

            if (w <= 0 || h <= 0)
                return;

            RECT bounds{ x, y, x + w, y + h };
            if (bounds.left == _lastBounds.left && bounds.top == _lastBounds.top &&
                bounds.right == _lastBounds.right && bounds.bottom == _lastBounds.bottom)
            {
                return;
            }
            _lastBounds = bounds;
            _controller->put_Bounds(bounds);
        }
        catch (...)
        {
        }
    }

    void WebViewPaneContent::_SetUrlText(const winrt::hstring& text)
    {
        if (_urlBox)
        {
            _urlBox.Text(text);
        }
    }

    void WebViewPaneContent::Navigate(const winrt::hstring& url)
    {
        std::wstring urlStr(url);
        if (urlStr.empty())
            return;
        if (urlStr.find(L"://") == std::wstring::npos)
            urlStr = L"http://" + urlStr;

        if (_webview)
        {
            _webview->Navigate(urlStr.c_str());
        }
        else
        {
            _url = winrt::hstring{ urlStr };
        }
    }

    void WebViewPaneContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
    }

    FrameworkElement WebViewPaneContent::GetRoot()
    {
        return _grid;
    }

    Size WebViewPaneContent::MinimumSize()
    {
        return { 200, 200 };
    }

    void WebViewPaneContent::Focus(FocusState reason)
    {
        if (reason != FocusState::Unfocused && _controller)
        {
            _controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }

    void WebViewPaneContent::Close()
    {
        _closed = true;
        if (_controller)
        {
            _controller->Close();
            _controller = nullptr;
        }
        _webview = nullptr;
    }

    INewContentArgs WebViewPaneContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"WEB:" + _url);
    }

    winrt::hstring WebViewPaneContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE774" }; // Globe
        return winrt::hstring{ glyph };
    }

    IReference<winrt::Windows::UI::Color> WebViewPaneContent::TabColor() const noexcept
    {
        return nullptr;
    }

    winrt::Windows::UI::Xaml::Media::Brush WebViewPaneContent::BackgroundBrush()
    {
        return nullptr;
    }
}
