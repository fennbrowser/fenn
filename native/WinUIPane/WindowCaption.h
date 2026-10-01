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
    // An optional element beside the system buttons, on the side they occupy.
    FrameworkElement trailing{nullptr};
    double trailingWidth{};
    HWND owner{};
    bool previousExtended{}, visible{};
    Height previousHeight{};
    Theme previousTheme{};
    Windows::Foundation::IReference<Windows::UI::Color> previousBackground{nullptr}, previousInactiveBackground{nullptr};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    void Open(HWND hwnd, Controls::Grid const& grid, FrameworkElement const& element = nullptr, double width = 0) {
        owner = hwnd;
        trailing = element;
        trailingWidth = width;
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
        if (trailing) grid.Children().Append(trailing);
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
            if (trailing) trailing.Visibility(Visibility::Visible);
            Update();
        } else {
            RemovePropW(owner, WINUI_PANE_SYSTEM_CAPTION_PROPERTY);
            if (trailing) trailing.Visibility(Visibility::Collapsed);
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
            // System caption buttons are 32 DIPs, or 48 in the tall layout.
            const double captionHeight = option == Height::Tall ? 48 : 32;
            if (trailing && trailing.Height() != captionHeight) trailing.Height(captionHeight);
        }
        const auto theme = root.ActualTheme() == ElementTheme::Dark ? Theme::Dark : Theme::Light;
        if (bar.PreferredTheme() != theme) bar.PreferredTheme(theme);
        const auto scale = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : GetDpiForWindow(owner) / 96.0;
        const bool rtl = root.FlowDirection() == FlowDirection::RightToLeft;
        // File Explorer leaves a wider empty drag area before its trailing controls.
        constexpr double kDragGap = 40;
        const double extra = trailing ? trailingWidth + kDragGap : 0;
        const double left = (rtl ? bar.RightInset() : bar.LeftInset()) / scale + (rtl ? extra : 0);
        const double right = (rtl ? bar.LeftInset() : bar.RightInset()) / scale + (rtl ? 0 : extra);
        if (trailing) {
            // The reserved column holds this element next to the caption buttons.
            const int column = rtl ? 0 : 2;
            if (Controls::Grid::GetColumn(trailing) != column) Controls::Grid::SetColumn(trailing, column);
            const auto side = rtl ? HorizontalAlignment::Right : HorizontalAlignment::Left;
            if (trailing.HorizontalAlignment() != side) trailing.HorizontalAlignment(side);
            // The gap sits before the button, which stays grouped with the system buttons.
            const Thickness margin = rtl ? Thickness{0, 0, kDragGap, 0} : Thickness{kDragGap, 0, 0, 0};
            const auto current = trailing.Margin();
            if (current.Left != margin.Left || current.Right != margin.Right) trailing.Margin(margin);
        }
        if (leftInset.Width().Value != left) leftInset.Width({left, GridUnitType::Pixel});
        if (rightInset.Width().Value != right) rightInset.Width({right, GridUnitType::Pixel});
    }
    void Close(bool restore = true) noexcept {
        lifetime.reset();
        if (owner) RemovePropW(owner, WINUI_PANE_SYSTEM_CAPTION_PROPERTY);
        try { if (restore && owner && IsWindow(owner)) SetVisible(false); } catch (...) {}
        trailing = nullptr;
        bar = nullptr;
        owner = nullptr;
    }
    ~NativeWindowCaption() { Close(); }
};
