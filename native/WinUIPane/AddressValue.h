#pragma once
#include <winrt/Microsoft.UI.Xaml.Documents.h>

// A noninteractive display layer; the stock TextBox still owns input, selection,
// clipboard, accessibility and its template. Firefox supplies UTF-16 offsets.
struct NativeAddressValue {
    winrt::Microsoft::UI::Xaml::Controls::TextBlock display{nullptr};
    winrt::Microsoft::UI::Xaml::UIElement content{nullptr};
    winrt::hstring text;
    bool available{};

    static void Validate(winrt::Windows::Data::Json::JsonObject const& snapshot) {
        using namespace winrt::Windows::Data::Json;
        if (!snapshot.HasKey(L"domainEmphasis") ||
            snapshot.GetNamedValue(L"domainEmphasis").ValueType() == JsonValueType::Null)
            return;
        auto value = snapshot.GetNamedObject(L"domainEmphasis");
        auto text = value.GetNamedString(L"text");
        auto start = value.GetNamedNumber(L"start"), end = value.GetNamedNumber(L"end");
        if (text.size() > NativeJson::kMaxFieldText || !std::isfinite(start) || !std::isfinite(end) ||
            std::floor(start) != start || std::floor(end) != end || start < 0 || end <= start || end > text.size())
            winrt::throw_hresult(E_INVALIDARG);
        // UTF-16 ranges must not split a surrogate pair.
        for (auto offset : {uint32_t(start), uint32_t(end)})
            if (offset && offset < text.size() && text[offset] >= 0xdc00 && text[offset] <= 0xdfff &&
                text[offset - 1] >= 0xd800 && text[offset - 1] <= 0xdbff)
                winrt::throw_hresult(E_INVALIDARG);
    }

    void Create() {
        using namespace winrt::Microsoft::UI::Xaml;
        display = Markup::XamlReader::Load(
                      LR"(
            <TextBlock xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                IsHitTestVisible="False" IsTextSelectionEnabled="False" Visibility="Collapsed"
                AutomationProperties.AccessibilityView="Raw" VerticalAlignment="Center"
                TextWrapping="NoWrap" TextTrimming="CharacterEllipsis" TextAlignment="Left"
                TextReadingOrder="DetectFromContent" FlowDirection="LeftToRight"><Run Foreground="{ThemeResource TextFillColorSecondaryBrush}"/><Run Foreground="{ThemeResource TextFillColorPrimaryBrush}"/><Run Foreground="{ThemeResource TextFillColorSecondaryBrush}"/></TextBlock>)")
                      .as<Controls::TextBlock>();
        Automation::AutomationProperties::SetAutomationId(display, L"FennAddressDisplay");
    }

    void Hide() {
        if (display) display.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        if (content) content.Opacity(1);
    }

    void Apply(winrt::Windows::Data::Json::JsonObject const& snapshot) {
        using namespace winrt::Windows::Data::Json;
        available = snapshot.HasKey(L"domainEmphasis") &&
                    snapshot.GetNamedValue(L"domainEmphasis").ValueType() == JsonValueType::Object;
        if (!available) {
            Hide();
            return;
        }
        auto value = snapshot.GetNamedObject(L"domainEmphasis");
        text = value.GetNamedString(L"text");
        size_t start = size_t(value.GetNamedNumber(L"start")), end = size_t(value.GetNamedNumber(L"end"));
        std::wstring valueText(text);
        const std::wstring parts[] = {valueText.substr(0, start), valueText.substr(start, end - start),
                                      valueText.substr(end)};
        for (uint32_t i = 0; i < 3; ++i)
            display.Inlines().GetAt(i).as<winrt::Microsoft::UI::Xaml::Documents::Run>().Text(parts[i]);
    }

    void Refresh(winrt::Microsoft::UI::Xaml::Controls::TextBox const& field, bool editing) {
        using namespace winrt::Microsoft::UI::Xaml;
        if (!display) return;
        if (!content) {
            field.ApplyTemplate();
            if (Media::VisualTreeHelper::GetChildrenCount(field)) {
                auto root = Media::VisualTreeHelper::GetChild(field, 0).try_as<FrameworkElement>();
                if (root) content = root.FindName(L"ContentElement").try_as<UIElement>();
            }
        }
        if (!content || !available || editing || field.SelectionLength() || field.Text() != text) {
            Hide();
            return;
        }
        auto padding = field.Padding(), border = field.BorderThickness();
        display.Margin({padding.Left + border.Left, 0, padding.Right + border.Right, 0});
        display.FontFamily(field.FontFamily());
        display.FontSize(field.FontSize());
        display.FontWeight(field.FontWeight());
        content.Opacity(0);
        display.Visibility(Visibility::Visible);
    }
};
