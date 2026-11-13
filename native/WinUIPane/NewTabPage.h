#pragma once
#include "JsonValidation.h"

// Window-local WinUI presentation only. The address editor is a sibling with a
// permanent parent; Firefox supplies eligibility, labels and opaque actions.
struct NativeNewTabPage {
    using Object = winrt::Windows::Data::Json::JsonObject;
    Microsoft::UI::Xaml::Controls::StackPanel heading{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock name{nullptr};
    Microsoft::UI::Xaml::Controls::Image logo{nullptr};
    Microsoft::UI::Xaml::Controls::Grid shortcuts{nullptr};
    Microsoft::UI::Xaml::Controls::ScrollViewer shortcutsScroll{nullptr};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::function<void(winrt::hstring const&)> activate;
    bool enabled{};
    Object state;
    ~NativeNewTabPage() { lifetime.reset(); }

    static Object Read(Object const& snapshot) {
        auto value = snapshot.GetNamedObject(L"newTab", Object());
        value.GetNamedBoolean(L"animate", false);
        if (!value.GetNamedBoolean(L"enabled", false)) return value;
        NativeJson::RequireLength(value.GetNamedString(L"name"), 128);
        NativeJson::RequireLength(value.GetNamedString(L"shortcutsLabel"), 512);
        auto rows = value.GetNamedArray(L"shortcuts");
        if (rows.Size() > 8) winrt::throw_hresult(E_INVALIDARG);
        size_t pixels{};
        NativeJson::ValidateImage(value, pixels, {128, 24576});
        std::set<winrt::hstring> ids;
        for (auto row : rows) {
            auto item = row.GetObject();
            auto id = item.GetNamedString(L"id");
            if (id.empty() || id.size() > 64 || !ids.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(item.GetNamedString(L"label"), 256);
            NativeJson::ValidateImage(item, pixels, {32, 24576});
        }
        return value;
    }
    void Create(Microsoft::UI::Xaml::Controls::Canvas const& canvas) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        heading = StackPanel();
        heading.Spacing(12);
        logo = Image();
        logo.Width(96);
        logo.Height(96);
        Automation::AutomationProperties::SetAutomationId(logo, L"FennNewTabLogo");
        heading.Children().Append(logo);
        name = TextBlock();
        name.FontSize(28);
        name.HorizontalAlignment(HorizontalAlignment::Center);
        heading.Children().Append(name);
        shortcuts = Grid();
        shortcutsScroll = ScrollViewer();
        shortcutsScroll.Content(shortcuts);
        shortcutsScroll.HorizontalScrollMode(ScrollMode::Disabled);
        shortcutsScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        shortcutsScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        Automation::AutomationProperties::SetAutomationId(heading, L"FennNewTabHeading");
        Automation::AutomationProperties::SetAutomationId(shortcuts, L"FennNewTabShortcuts");
        canvas.Children().Append(heading);
        canvas.Children().Append(shortcutsScroll);
        heading.Visibility(Visibility::Collapsed);
        shortcutsScroll.Visibility(Visibility::Collapsed);
    }
    static void ApplyIcon(Microsoft::UI::Xaml::Controls::Grid const& slot, Object const& item) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const bool present = item.HasKey(L"image") && item.GetNamedValue(L"image").ValueType() !=
                                                          winrt::Windows::Data::Json::JsonValueType::Null;
        auto icon = slot.Children().GetAt(0).as<Image>();
        icon.Source(present ? NativeJson::DecodeImage(item.GetNamedObject(L"image")) : nullptr);
        icon.Visibility(present ? Visibility::Visible : Visibility::Collapsed);
        slot.Children().GetAt(1).Visibility(present ? Visibility::Collapsed : Visibility::Visible);
    }
    void Apply(Object const& value) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        enabled = value.GetNamedBoolean(L"enabled", false);
        heading.Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
        shortcutsScroll.Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
        if (state.Stringify() == value.Stringify()) return;
        bool sameRows = enabled && state.GetNamedBoolean(L"enabled", false);
        if (sameRows) {
            auto before = state.GetNamedArray(L"shortcuts"), after = value.GetNamedArray(L"shortcuts");
            sameRows = before.Size() == after.Size();
            for (uint32_t i = 0; sameRows && i < before.Size(); ++i)
                sameRows = before.GetObjectAt(i).GetNamedString(L"id") == after.GetObjectAt(i).GetNamedString(L"id");
        }
        state = value;
        if (!sameRows) shortcuts.Children().Clear();
        if (!enabled) return;
        name.Text(value.GetNamedString(L"name"));
        if (value.HasKey(L"image") &&
            value.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null)
            logo.Source(NativeJson::DecodeImage(value.GetNamedObject(L"image")));
        Automation::AutomationProperties::SetName(shortcuts, value.GetNamedString(L"shortcutsLabel"));
        uint32_t index{};
        for (auto row : value.GetNamedArray(L"shortcuts")) {
            auto item = row.GetObject();
            auto id = item.GetNamedString(L"id");
            if (sameRows) {
                auto button = shortcuts.Children().GetAt(index++).as<Button>();
                auto contents = button.Content().as<StackPanel>();
                contents.Children().GetAt(1).as<TextBlock>().Text(item.GetNamedString(L"label"));
                ApplyIcon(contents.Children().GetAt(0).as<Grid>(), item);
                continue;
            }
            Button button;
            StackPanel contents;
            contents.Spacing(8);
            Grid iconSlot;
            iconSlot.Width(32);
            iconSlot.Height(32);
            Image icon;
            FontIcon fallback;
            fallback.Glyph(L"\xE774");
            fallback.FontSize(24);
            iconSlot.Children().Append(icon);
            iconSlot.Children().Append(fallback);
            ApplyIcon(iconSlot, item);
            TextBlock label;
            label.Text(item.GetNamedString(L"label"));
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            contents.Children().Append(iconSlot);
            contents.Children().Append(label);
            button.Content(contents);
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.Margin({4, 4, 4, 4});
            button.Height(80);
            Automation::AutomationProperties::SetName(button, item.GetNamedString(L"label"));
            Automation::AutomationProperties::SetAutomationId(button, L"FennNewTabShortcut-" + id);
            button.Click([this, id, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired() && enabled && activate) activate(id);
            });
            shortcuts.Children().Append(button);
        }
    }
    void Arrange(double width, double height, double editorTop, double editorWidth) {
        using namespace Microsoft::UI::Xaml::Controls;
        heading.Width(editorWidth);
        Canvas::SetLeft(heading, (width - editorWidth) / 2);
        Canvas::SetTop(heading, editorTop - 168);
        shortcuts.Width(editorWidth);
        shortcutsScroll.Width(editorWidth);
        shortcutsScroll.MaxHeight((std::max)(0.0, height - editorTop - 72));
        Canvas::SetLeft(shortcutsScroll, (width - editorWidth) / 2);
        Canvas::SetTop(shortcutsScroll, editorTop + 56);
        const uint32_t columns = editorWidth >= 560 ? 4 : editorWidth >= 360 ? 3 : 2;
        if (shortcuts.ColumnDefinitions().Size() != columns) {
            shortcuts.ColumnDefinitions().Clear();
            for (uint32_t i = 0; i < columns; ++i) shortcuts.ColumnDefinitions().Append(ColumnDefinition());
        }
        const uint32_t rows = (shortcuts.Children().Size() + columns - 1) / columns;
        if (shortcuts.RowDefinitions().Size() != rows) {
            shortcuts.RowDefinitions().Clear();
            for (uint32_t i = 0; i < rows; ++i) shortcuts.RowDefinitions().Append(RowDefinition());
        }
        for (uint32_t i = 0; i < shortcuts.Children().Size(); ++i) {
            auto child = shortcuts.Children().GetAt(i).as<Microsoft::UI::Xaml::FrameworkElement>();
            Grid::SetRow(child, i / columns);
            Grid::SetColumn(child, i % columns);
        }
    }
};
