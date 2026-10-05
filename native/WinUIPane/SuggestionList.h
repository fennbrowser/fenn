#pragma once
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include "JsonValidation.h"

// The address suggestion rows. Presentation only: Firefox owns ranking,
// titles, and which row (if any) is selected; this only renders rows and
// exposes their identities. Selection gestures and the pending-action bus
// they feed stay with the owning NativeAddress, since Action() must read
// the selected row directly to build outgoing messages.
struct SuggestionList {
    using JObject = winrt::Windows::Data::Json::JsonObject;

    std::function<void(winrt::hstring const&, winrt::hstring const&)> activateButton;
    std::function<void()> focusField;
    std::function<void(winrt::hstring const&, winrt::hstring const&, Microsoft::UI::Xaml::FrameworkElement const&)> openButtonMenu;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~SuggestionList() { lifetime.reset(); }
    Microsoft::UI::Xaml::Controls::ListView list;
    std::vector<winrt::hstring> ids, titles, actionLabels;
    struct RowVisual {
        Microsoft::UI::Xaml::Controls::TextBlock title, section, description, url, feedback;
        Microsoft::UI::Xaml::Controls::Image image;
        Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
        Microsoft::UI::Xaml::Controls::StackPanel buttons;
        std::vector<winrt::hstring> buttonIds;
        std::vector<Microsoft::UI::Xaml::Controls::Button> mainButtons, menuButtons;
        Microsoft::UI::Xaml::Documents::Hyperlink help{nullptr};
        winrt::hstring helpKey, pixels;
        int pixelWidth{}, pixelHeight{};
    };
    std::vector<RowVisual> visuals;
    winrt::hstring pendingFocus, lastFocus;

    SuggestionList() {
        using namespace Microsoft::UI::Xaml::Controls;
        Automation::AutomationProperties::SetName(list, L"Address suggestions");
        Automation::AutomationProperties::SetAutomationId(list, L"FirefoxWinUISuggestions");
        list.MaxHeight(160);
        list.SelectionMode(ListViewSelectionMode::Single);
    }

    Microsoft::UI::Xaml::Controls::Button FirstButton() const {
        for (auto const& row : visuals)
            for (auto const& button : row.mainButtons) {
                if (button.IsEnabled()) return button;
            }
        return nullptr;
    }
    Microsoft::UI::Xaml::Documents::Hyperlink FirstHelp() const {
        for (auto const& row : visuals) {
            if (row.help) return row.help;
            for (auto const& button : row.mainButtons) if (button.IsEnabled()) return nullptr;
        }
        return nullptr;
    }
    bool FocusFirstAction() const {
        for (auto const& row : visuals) {
            if (row.help) return row.help.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
            for (auto const& button : row.mainButtons)
                if (button.IsEnabled()) return button.Focus(Microsoft::UI::Xaml::FocusState::Keyboard);
        }
        return false;
    }
    void FocusPending() {
        if (pendingFocus.empty()) return;
        for (auto const& row : visuals)
            for (uint32_t i = 0; i < row.buttonIds.size(); ++i)
                if (row.buttonIds[i] == pendingFocus) {
                    row.mainButtons[i].Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
                    pendingFocus = L""; return;
                }
        pendingFocus = L"";
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
            auto name = visuals[i].section.Text().empty() ? title : visuals[i].section.Text() + L". " + title;
            if (!visuals[i].description.Text().empty()) name = name + L". " + visuals[i].description.Text();
            if (!visuals[i].url.Text().empty()) name = name + L". " + visuals[i].url.Text();
            if (!visuals[i].feedback.Text().empty() && visuals[i].feedback.Text() != title) name = name + L". " + visuals[i].feedback.Text();
            Automation::AutomationProperties::SetName(container, name);
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
                StackPanel text;
                text.Spacing(2);
                text.Children().Append(visual.title);
                visual.description.TextWrapping(TextWrapping::Wrap);
                visual.description.MaxLines(2);
                visual.description.TextTrimming(TextTrimming::CharacterEllipsis);
                visual.url.TextTrimming(TextTrimming::CharacterEllipsis);
                text.Children().Append(visual.description);
                text.Children().Append(visual.url);
                visual.feedback.TextWrapping(TextWrapping::Wrap);
                Automation::AutomationProperties::SetLiveSetting(visual.feedback, Automation::Peers::AutomationLiveSetting::Polite);
                text.Children().Append(visual.feedback);
                visual.buttons.Orientation(Orientation::Vertical);
                visual.buttons.Spacing(4);
                text.Children().Append(visual.buttons);
                Grid::SetColumn(text, 1);
                Grid icon;
                icon.Children().Append(visual.symbol);
                icon.Children().Append(visual.image);
                visual.image.Stretch(Media::Stretch::Uniform);
                Automation::AutomationProperties::SetAccessibilityView(visual.image,
                                                                       Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(visual.symbol,
                                                                       Automation::Peers::AccessibilityView::Raw);
                line.Children().Append(icon);
                line.Children().Append(text);
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
            auto buttons = row.GetNamedArray(L"buttons", winrt::Windows::Data::Json::JsonArray());
            bool sameButtons = buttons.Size() == visual.buttonIds.size();
            for (uint32_t b = 0; sameButtons && b < buttons.Size(); ++b)
                sameButtons = buttons.GetAt(b).GetObject().GetNamedString(L"id") == visual.buttonIds[b];
            if (!sameButtons) {
                visual.buttons.Children().Clear(); visual.buttonIds.clear();
                visual.mainButtons.clear(); visual.menuButtons.clear();
                for (auto value : buttons) {
                    const auto id = value.GetObject().GetNamedString(L"id"), resultId = ids[i];
                    Button button; button.HorizontalAlignment(HorizontalAlignment::Left);
                    TextBlock label; label.TextWrapping(TextWrapping::Wrap); label.MaxWidth(220); button.Content(label);
                    Automation::AutomationProperties::SetAutomationId(button, L"FirefoxWinUITip-" + id);
                    button.Click([this, id, resultId, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (!alive.expired() && activateButton) activateButton(id, resultId);
                    });
                    button.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto const& e) {
                        if (!alive.expired() && e.Key() == winrt::Windows::System::VirtualKey::Tab &&
                            (GetKeyState(VK_SHIFT) & 0x8000) && sender == FirstButton() && !FirstHelp() && focusField) {
                            focusField(); e.Handled(true);
                        }
                    });
                    StackPanel group; group.Orientation(Orientation::Horizontal); group.Spacing(0);
                    group.Children().Append(button);
                    Button menuButton; menuButton.VerticalAlignment(VerticalAlignment::Stretch);
                    FontIcon chevron; chevron.Glyph(L"\uE70D"); chevron.FontSize(10); menuButton.Content(chevron);
                    Automation::AutomationProperties::SetAutomationId(menuButton, L"FirefoxWinUITipMenu-" + id);
                    menuButton.Click([this, id, resultId, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto const&) {
                        if (!alive.expired() && openButtonMenu) openButtonMenu(id, resultId, sender.template as<FrameworkElement>());
                    });
                    group.Children().Append(menuButton);
                    visual.buttons.Children().Append(group); visual.buttonIds.push_back(id);
                    visual.mainButtons.push_back(button); visual.menuButtons.push_back(menuButton);
                }
            }
            for (uint32_t b = 0; b < buttons.Size(); ++b) {
                auto data = buttons.GetAt(b).GetObject();
                auto button = visual.mainButtons[b];
                auto menuButton = visual.menuButtons[b];
                menuButton.Visibility(data.GetNamedBoolean(L"menu", false) ? Visibility::Visible : Visibility::Collapsed);
                menuButton.IsEnabled(data.GetNamedBoolean(L"enabled"));
                Automation::AutomationProperties::SetName(menuButton, data.GetNamedString(L"menuLabel", L""));
                auto label = data.GetNamedString(L"label");
                button.Content().as<TextBlock>().Text(label);
                button.IsEnabled(data.GetNamedBoolean(L"enabled"));
                Automation::AutomationProperties::SetName(button, label);
            }
            if (buttons.Size() && row.GetNamedBoolean(L"focusButton", false) && visual.buttonIds[0] != lastFocus) {
                pendingFocus = lastFocus = visual.buttonIds[0];
            }
            visual.buttons.Visibility(buttons.Size() ? Visibility::Visible : Visibility::Collapsed);
            const auto feedback = row.GetNamedString(L"feedback", L"");
            if (visual.feedback.Text() != feedback) {
                visual.feedback.Text(feedback);
                const bool acknowledgment = feedback == row.GetNamedString(L"title");
                auto target = acknowledgment ? visual.title : visual.feedback;
                if (acknowledgment) target.Text(feedback);
                Automation::AutomationProperties::SetLiveSetting(target, Automation::Peers::AutomationLiveSetting::Polite);
                if (auto peer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(target))
                    peer.RaiseAutomationEvent(Automation::Peers::AutomationEvents::LiveRegionChanged);
            }
            visual.feedback.Visibility(feedback.empty() || feedback == row.GetNamedString(L"title") ? Visibility::Collapsed : Visibility::Visible);
            const auto description = row.GetNamedString(L"description", L"");
            const auto url = row.GetNamedString(L"displayUrl", L"");
            if (row.HasKey(L"helpLink")) {
                auto help = row.GetNamedObject(L"helpLink");
                auto key = help.Stringify();
                if (key != visual.helpKey) {
                    visual.description.Inlines().Clear();
                    Documents::Run before, label, after;
                    before.Text(help.GetNamedString(L"before")); label.Text(help.GetNamedString(L"label")); after.Text(help.GetNamedString(L"after"));
                    visual.help = Documents::Hyperlink();
                    visual.help.Inlines().Append(label);
                    const auto id = help.GetNamedString(L"id"), resultId = ids[i];
                    Automation::AutomationProperties::SetAutomationId(visual.help, L"FirefoxWinUIHelp-" + id);
                    Automation::AutomationProperties::SetName(visual.help, help.GetNamedString(L"label"));
                    visual.help.Click([this, id, resultId, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (!alive.expired() && activateButton) activateButton(id, resultId);
                    });
                    visual.description.Inlines().Append(before); visual.description.Inlines().Append(visual.help); visual.description.Inlines().Append(after);
                    visual.helpKey = key;
                }
                visual.description.MaxLines(0); // Keep the interactive link reachable when text wraps.
            } else {
                visual.help = nullptr; visual.helpKey = L"";
                visual.description.Text(description); visual.description.MaxLines(2);
            }
            visual.description.Visibility(description.empty() ? Visibility::Collapsed : Visibility::Visible);
            visual.url.Text(url == titles[i] ? L"" : url);
            visual.url.Visibility(visual.url.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
            visual.title.FlowDirection(row.GetNamedBoolean(L"titleLtr", false)
                                           ? FlowDirection::LeftToRight : list.FlowDirection());
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
                                 : symbol == L"copy"    ? Symbol::Copy
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
