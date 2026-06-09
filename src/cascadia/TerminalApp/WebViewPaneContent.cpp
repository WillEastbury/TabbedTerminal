// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WebViewPaneContent.h"
#include "Utils.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Microsoft::Terminal::Settings::Model;

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

        // Two rows: URL bar (Auto) + WebView (*)
        WUX::Controls::RowDefinition urlRow;
        urlRow.Height(WUX::GridLengthHelper::Auto());
        WUX::Controls::RowDefinition contentRow;
        contentRow.Height(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
        _grid.RowDefinitions().Append(urlRow);
        _grid.RowDefinitions().Append(contentRow);

        // URL bar: [Back] [Forward] [Refresh] [URL TextBox] [Go]
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

        auto backBtn = makeNavButton(L"\xE72B");     // ChevronLeft
        auto fwdBtn = makeNavButton(L"\xE72A");      // ChevronRight
        auto refreshBtn = makeNavButton(L"\xE72C");   // Refresh
        auto homeBtn = makeNavButton(L"\xE80F");      // Home

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

        // WebView
        _webView = WUX::Controls::WebView();
        _webView.Source(Uri(_url));
        WUX::Controls::Grid::SetRow(_webView, 1);
        _grid.Children().Append(_webView);

        // Wire navigation
        auto webViewWeak = winrt::make_weak(_webView);
        auto urlBoxWeak = winrt::make_weak(_urlBox);
        auto weakThis = winrt::make_weak<IPaneContent>(*this);

        auto navigateFromBox = [webViewWeak, urlBoxWeak]() {
            auto wv = webViewWeak.get();
            auto ub = urlBoxWeak.get();
            if (wv && ub)
            {
                std::wstring urlStr(ub.Text());
                if (urlStr.empty())
                    return;
                if (urlStr.find(L"://") == std::wstring::npos)
                    urlStr = L"http://" + urlStr;
                try
                {
                    wv.Navigate(Uri(urlStr));
                }
                catch (...)
                {
                    // Invalid URL — ignore rather than throw out of the handler
                }
            }
        };

        backBtn.Click([webViewWeak](auto&&, auto&&) {
            if (auto wv = webViewWeak.get()) { if (wv.CanGoBack()) wv.GoBack(); }
        });
        fwdBtn.Click([webViewWeak](auto&&, auto&&) {
            if (auto wv = webViewWeak.get()) { if (wv.CanGoForward()) wv.GoForward(); }
        });
        refreshBtn.Click([webViewWeak](auto&&, auto&&) {
            if (auto wv = webViewWeak.get()) { wv.Refresh(); }
        });
        homeBtn.Click([webViewWeak, url = _url](auto&&, auto&&) {
            if (auto wv = webViewWeak.get())
            {
                try { wv.Navigate(Uri(url)); }
                catch (...) {}
            }
        });
        goBtn.Click([navigateFromBox](auto&&, auto&&) { navigateFromBox(); });

        _urlBox.KeyDown([navigateFromBox](auto&&, const WUX::Input::KeyRoutedEventArgs& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
                navigateFromBox();
        });

        // Update URL box and title on navigation
        _webView.NavigationCompleted([urlBoxWeak, weakThis](auto&& sender, auto&&) {
            if (auto ub = urlBoxWeak.get())
            {
                if (auto wv = sender.try_as<WUX::Controls::WebView>())
                {
                    ub.Text(wv.Source().AbsoluteUri());
                }
            }
        });

        _webView.NavigationStarting([urlBoxWeak](auto&& sender, auto&&) {
            if (auto ub = urlBoxWeak.get())
            {
                if (auto wv = sender.try_as<WUX::Controls::WebView>())
                {
                    ub.Text(wv.Source().AbsoluteUri());
                }
            }
        });
    }

    void WebViewPaneContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
        // No settings to update for WebView
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
        if (reason != FocusState::Unfocused && _webView)
        {
            _webView.Focus(reason);
        }
    }

    void WebViewPaneContent::Close()
    {
        if (_webView)
        {
            // Navigate away to release resources
            _webView.NavigateToString(L"");
        }
    }

    INewContentArgs WebViewPaneContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"WEB:" + _url);
    }

    winrt::hstring WebViewPaneContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE774" }; // Globe icon
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

    void WebViewPaneContent::Navigate(const winrt::hstring& url)
    {
        _url = url;
        if (_webView)
        {
            _webView.Source(Uri(url));
        }
        if (_urlBox)
        {
            _urlBox.Text(url);
        }
    }
}
