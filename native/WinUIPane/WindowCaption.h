#pragma once
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>

// Windows owns caption drawing, input, accessibility and Snap Layouts. The
// island only reserves their space; never imitate these controls with Buttons.
struct NativeWindowCaption {
    using TitleBar = Microsoft::UI::Windowing::AppWindowTitleBar;
    using Height = Microsoft::UI::Windowing::TitleBarHeightOption;
    using Theme = Microsoft::UI::Windowing::TitleBarTheme;
    TitleBar bar{nullptr};
    Controls::Grid root{nullptr};
    Controls::ColumnDefinition leftInset{nullptr}, rightInset{nullptr};
    Controls::Border leftBottomLine{nullptr}, rightBottomLine{nullptr};
    HWND owner{};
    bool previousExtended{}, visible{};
    Height previousHeight{};
    Theme previousTheme{};
    Windows::Foundation::IReference<Windows::UI::Color> previousBackground{nullptr}, previousInactiveBackground{nullptr};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    void Open(HWND hwnd, Controls::Grid const& grid) {
        owner = hwnd;
        root = grid;
        bar = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
            Microsoft::UI::GetWindowIdFromWindow(hwnd)).TitleBar();
        previousExtended = bar.ExtendsContentIntoTitleBar();
        previousHeight = bar.PreferredHeightOption();
        previousTheme = bar.PreferredTheme();
        previousBackground = bar.ButtonBackgroundColor();
        previousInactiveBackground = bar.ButtonInactiveBackgroundColor();
        Controls::ColumnDefinition tabs;
        tabs.Width({1, GridUnitType::Star});
        leftInset = Controls::ColumnDefinition{};
        rightInset = Controls::ColumnDefinition{};
        leftInset.Width({0, GridUnitType::Pixel});
        rightInset.Width({0, GridUnitType::Pixel});
        grid.ColumnDefinitions().Append(leftInset);
        grid.ColumnDefinitions().Append(tabs);
        grid.ColumnDefinitions().Append(rightInset);
        Controls::Grid::SetColumn(grid.Children().GetAt(0).as<FrameworkElement>(), 1);
        // Continue TabView's themed bottom edge beneath the system captions
        // in the 40-DIP layout without covering the selected tab's open edge.
        auto makeBottomLine = [&](int column) {
            auto line = Markup::XamlReader::Load(
                LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource TabViewBorderBrush}" Height="1" VerticalAlignment="Bottom" IsHitTestVisible="False" Visibility="Collapsed" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
            Controls::Grid::SetColumn(line, column);
            grid.Children().Append(line);
            return line;
        };
        leftBottomLine = makeBottomLine(0);
        rightBottomLine = makeBottomLine(2);
        auto weak = std::weak_ptr<int>(lifetime);
        root.ActualThemeChanged([this, weak](auto const&, auto const&) {
            // Read the settled theme after XAML finishes propagating it through
            // the tree; decorative children can change notification ordering.
            if (!weak.expired()) root.DispatcherQueue().TryEnqueue([this, weak] {
                if (!weak.expired()) Update();
            });
        });
    }
    void SetVisible(bool value) {
        if (!bar || visible == value) return;
        visible = value;
        if (value) {
            check_bool(SetPropW(owner, WINUI_PANE_SYSTEM_CAPTION_PROPERTY, reinterpret_cast<HANDLE>(1)));
            bar.ExtendsContentIntoTitleBar(true);
            // Start with the default; Update selects Tall for a 48-DIP host.
            bar.PreferredHeightOption(Height::Standard);
            // Reveal the island's live themed background. Windows retains all
            // hover/pressed colors, including Close red and high-contrast rules.
            bar.ButtonBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
            bar.ButtonInactiveBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
            Update();
        } else {
            RemovePropW(owner, WINUI_PANE_SYSTEM_CAPTION_PROPERTY);
            bar.PreferredHeightOption(previousHeight);
            bar.ExtendsContentIntoTitleBar(previousExtended);
            bar.PreferredTheme(previousTheme);
            bar.ButtonBackgroundColor(previousBackground);
            bar.ButtonInactiveBackgroundColor(previousInactiveBackground);
        }
    }
    void Update() {
        if (!bar || !visible) return;
        // The Firefox reservation is the layout contract, in island DIPs.
        // Keep 32-DIP tab items bottom-aligned in all three supported heights.
        const double height = root.ActualHeight();
        const auto lineVisibility = height > 33 && height < 47 ? Visibility::Visible : Visibility::Collapsed;
        if (leftBottomLine.Visibility() != lineVisibility) {
            leftBottomLine.Visibility(lineVisibility);
            rightBottomLine.Visibility(lineVisibility);
        }
        if (height > 0) {
            const auto option = height >= 47 ? Height::Tall : Height::Standard;
            if (bar.PreferredHeightOption() != option) bar.PreferredHeightOption(option);
            auto tabs = root.Children().GetAt(0).as<Controls::TabView>();
            const double top = height >= 47 ? 16 : height <= 33 ? 0 : 8;
            if (tabs.Padding().Top != top) tabs.Padding({0, top, 0, 0});
        }
        const auto theme = root.ActualTheme() == ElementTheme::Dark ? Theme::Dark : Theme::Light;
        if (bar.PreferredTheme() != theme) bar.PreferredTheme(theme);
        const auto scale = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : GetDpiForWindow(owner) / 96.0;
        const bool rtl = root.FlowDirection() == FlowDirection::RightToLeft;
        const double left = (rtl ? bar.RightInset() : bar.LeftInset()) / scale;
        const double right = (rtl ? bar.LeftInset() : bar.RightInset()) / scale;
        if (leftInset.Width().Value != left) leftInset.Width({left, GridUnitType::Pixel});
        if (rightInset.Width().Value != right) rightInset.Width({right, GridUnitType::Pixel});
    }
    void Close(bool restore = true) noexcept {
        lifetime.reset();
        if (owner) RemovePropW(owner, WINUI_PANE_SYSTEM_CAPTION_PROPERTY);
        try { if (restore && owner && IsWindow(owner)) SetVisible(false); } catch (...) {}
        bar = nullptr;
        owner = nullptr;
    }
    ~NativeWindowCaption() { Close(); }
};
