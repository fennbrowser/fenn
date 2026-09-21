#pragma once
#include <winrt/Windows.Data.Json.h>
#include "JsonValidation.h"

// The address suggestion rows. Presentation only: Firefox owns ranking,
// titles, and which row (if any) is selected; this only renders rows and
// exposes their identities. Selection gestures and the pending-action bus
// they feed stay with the owning NativeAddress, since Action() must read
// the selected row directly to build outgoing messages.
struct SuggestionList {
    using JObject = winrt::Windows::Data::Json::JsonObject;

    Microsoft::UI::Xaml::Controls::ListView list;
    std::vector<winrt::hstring> ids, titles, actionLabels;
    struct RowVisual {
        Microsoft::UI::Xaml::Controls::TextBlock title, section;
        Microsoft::UI::Xaml::Controls::Image image;
        Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
        winrt::hstring pixels;
        int pixelWidth{}, pixelHeight{};
    };
    std::vector<RowVisual> visuals;

    SuggestionList() {
        using namespace Microsoft::UI::Xaml::Controls;
        Automation::AutomationProperties::SetName(list, L"Address suggestions");
        Automation::AutomationProperties::SetAutomationId(list, L"FirefoxWinUISuggestions");
        list.MaxHeight(160);
        list.SelectionMode(ListViewSelectionMode::Single);
    }

    void Clear() {
        ids.clear();
        visuals.clear();
        list.Items().Clear();
    }

    void RefreshLabels() {
        using namespace Microsoft::UI::Xaml::Controls;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto container = list.Items().GetAt(i).as<ListViewItem>();
            auto title = titles[i];
            if (int(i) == list.SelectedIndex() && !actionLabels[i].empty()) title = title + L" · " + actionLabels[i];
            visuals[i].title.Text(title);
            Automation::AutomationProperties::SetName(
                container, visuals[i].section.Text().empty() ? title : visuals[i].section.Text() + L". " + title);
        }
    }

    // Rebuilds rows only when the caller has determined the row set or
    // ordering may have changed (compared by sequence/revision); called
    // unconditionally would defeat that skip. Re-checks the size bound the
    // caller already validated, matching the snapshot's other fields.
    void SyncRows(winrt::Windows::Data::Json::JsonArray const& rows) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (rows.Size() > NativeJson::kMaxListEntries) winrt::throw_hresult(E_INVALIDARG);
        bool sameIds = rows.Size() == ids.size();
        for (uint32_t i = 0; sameIds && i < rows.Size(); ++i)
            sameIds = rows.GetAt(i).GetObject().GetNamedString(L"id") == ids[i];
        if (!sameIds) {
            ids.clear();
            visuals.clear();
            list.Items().Clear();
        }
        titles.clear();
        actionLabels.clear();
        for (auto item : rows) {
            auto row = item.GetObject();
            auto id = row.GetNamedString(L"id");
            auto title = row.GetNamedString(L"title");
            titles.push_back(title);
            actionLabels.push_back(row.GetNamedString(L"actionLabel", L""));
            if (!sameIds) {
                ids.push_back(id);
                ListViewItem container;
                RowVisual visual;
                visual.title.Text(title);
                visual.title.TextTrimming(TextTrimming::CharacterEllipsis);
                visual.title.VerticalAlignment(VerticalAlignment::Center);
                visual.section.TextWrapping(TextWrapping::Wrap);
                Automation::AutomationProperties::SetHeadingLevel(visual.section,
                                                                  Automation::Peers::AutomationHeadingLevel::Level2);
                StackPanel content;
                content.Spacing(4);
                Grid line;
                line.ColumnSpacing(8);
                ColumnDefinition iconColumn;
                iconColumn.Width(GridLengthHelper::Auto());
                ColumnDefinition titleColumn;
                titleColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                line.ColumnDefinitions().Append(iconColumn);
                line.ColumnDefinitions().Append(titleColumn);
                Grid::SetColumn(visual.title, 1);
                Grid icon;
                icon.Children().Append(visual.symbol);
                icon.Children().Append(visual.image);
                visual.image.Stretch(Media::Stretch::Uniform);
                Automation::AutomationProperties::SetAccessibilityView(visual.image,
                                                                       Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(visual.symbol,
                                                                       Automation::Peers::AccessibilityView::Raw);
                line.Children().Append(icon);
                line.Children().Append(visual.title);
                content.Children().Append(visual.section);
                content.Children().Append(line);
                container.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                container.Content(content);
                container.Tag(winrt::box_value(id));
                visuals.push_back(visual);
                list.Items().Append(container);
            }
        }
        for (uint32_t i = 0; i < rows.Size(); ++i) {
            auto row = rows.GetAt(i).GetObject();
            list.Items().GetAt(i).as<ListViewItem>().IsEnabled(row.GetNamedBoolean(L"enabled", true));
            auto& visual = visuals[i];
            const auto section = row.GetNamedString(L"sectionLabel", L"");
            visual.section.Text(section);
            visual.section.Visibility(section.empty() ? Visibility::Collapsed : Visibility::Visible);
            const auto size = row.GetNamedNumber(L"imageSize", 16);
            visual.image.Width(size);
            visual.image.Height(size);
            visual.symbol.Width(size);
            visual.symbol.Height(size);
            const auto symbol = row.GetNamedString(L"symbol", L"page");
            visual.symbol.Symbol(symbol == L"search"    ? Symbol::Find
                                 : symbol == L"history" ? Symbol::Clock
                                                        : Symbol::Document);
            const bool hasImage = row.HasKey(L"image") && row.GetNamedValue(L"image").ValueType() !=
                                                              winrt::Windows::Data::Json::JsonValueType::Null;
            if (hasImage) {
                auto data = row.GetNamedObject(L"image");
                auto pixels = data.GetNamedString(L"pixels");
                const int width = int(data.GetNamedNumber(L"width")), height = int(data.GetNamedNumber(L"height"));
                if (visual.pixels != pixels || visual.pixelWidth != width || visual.pixelHeight != height) {
                    visual.image.Source(NativeJson::DecodeImage(data));
                    visual.pixels = pixels;
                    visual.pixelWidth = width;
                    visual.pixelHeight = height;
                }
            } else {
                visual.image.Source(nullptr);
                visual.pixels = L"";
            }
            visual.image.Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
            visual.symbol.Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
        }
    }

    void SyncSelection(winrt::hstring const& selectedId) {
        auto selectedIt = std::find(ids.begin(), ids.end(), selectedId);
        list.SelectedIndex(selectedIt == ids.end() ? -1 : int(selectedIt - ids.begin()));
    }

    // The ids whose rows are actually within the list's viewport. Returns them
    // rather than appending to a JSON array: the caller compares this against
    // what it last reported, and building JSON to answer "did anything change"
    // is work done to discover that nothing has.
    std::vector<winrt::hstring> VisibleIds() const {
        using namespace Microsoft::UI::Xaml;
        std::vector<winrt::hstring> visible;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto row = list.ContainerFromIndex(i).try_as<FrameworkElement>();
            if (!row || row.ActualHeight() <= 0) continue;
            auto origin = row.TransformToVisual(list).TransformPoint({0, 0});
            if (origin.Y < list.ActualHeight() && origin.Y + row.ActualHeight() > 0)
                visible.push_back(ids[i]);
        }
        return visible;
    }
};
