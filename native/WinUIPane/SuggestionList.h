#pragma once
#include <limits>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Text.h>
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
    std::function<void(winrt::hstring const&, winrt::hstring const&, winrt::Microsoft::UI::Xaml::FrameworkElement const&)> openButtonMenu;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~SuggestionList() { lifetime.reset(); }
    winrt::Microsoft::UI::Xaml::Controls::ListView list;
    std::vector<winrt::hstring> ids, titles, actionLabels;
    std::vector<bool> current;
    std::vector<bool> inlineUrls;
    int retainedSelection = -1; // Appearance only; never an actionable selection.
    struct ActionBinding { winrt::hstring resultId, actionId; bool enabled = false; };
    struct RowVisual {
        winrt::Microsoft::UI::Xaml::Controls::TextBlock title, section, description, url, feedback, destination, separator, action;
        winrt::Microsoft::UI::Xaml::Controls::Grid headline;
        winrt::Microsoft::UI::Xaml::Controls::Image image;
        winrt::Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
        winrt::Microsoft::UI::Xaml::Controls::Viewbox symbolHost;
        winrt::Microsoft::UI::Xaml::Controls::StackPanel buttons;
        std::vector<winrt::hstring> buttonIds;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> mainButtons, menuButtons;
        std::vector<std::shared_ptr<ActionBinding>> bindings;
        std::shared_ptr<ActionBinding> helpBinding;
        winrt::hstring presentationId;
        winrt::Microsoft::UI::Xaml::Documents::Hyperlink help{nullptr};
        winrt::hstring helpKey, pixels;
        winrt::hstring titleRunsKey, urlRunsKey, destinationRunsKey;
        int pixelWidth{}, pixelHeight{};
    };
    std::vector<RowVisual> visuals;
    winrt::hstring pendingFocus, lastFocus;
    winrt::Windows::Foundation::IInspectable pendingControlFocus{nullptr};
    JObject arrowTargets;
    winrt::hstring ArrowTarget(bool reverse) const {
        const auto key = reverse ? L"backward" : L"forward";
        const int index = CurrentSelectedIndex();
        if (index >= 0) for (auto value : arrowTargets.GetNamedArray(L"rows", winrt::Windows::Data::Json::JsonArray())) {
            const auto row = value.GetObject();
            if (row.GetNamedString(L"id") == ids[index]) return row.GetNamedString(key);
        }
        return arrowTargets.GetNamedString(key, L"");
    }

    // Validate the copied presentation before any XAML mutation. Firefox has
    // already split its formatted text; native code does no matching.
    static void ValidateTextRuns(JObject const& row, wchar_t const* key, winrt::hstring const& text) {
        auto runs = row.GetNamedArray(key, winrt::Windows::Data::Json::JsonArray());
        if (runs.Size() > 129) winrt::throw_hresult(E_INVALIDARG);
        std::wstring joined;
        for (auto value : runs) {
            auto run = value.GetObject();
            auto part = run.GetNamedString(L"text");
            run.GetNamedBoolean(L"matched");
            if (part.empty() || part.size() > text.size() - joined.size()) winrt::throw_hresult(E_INVALIDARG);
            joined.append(part.c_str(), part.size());
        }
        if (runs.Size() && std::wstring_view(joined) != std::wstring_view(text.c_str(), text.size())) winrt::throw_hresult(E_INVALIDARG);
    }

    static void RenderText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& block,
                           winrt::hstring const& text, winrt::Windows::Data::Json::JsonArray const& runs,
                           winrt::hstring& previous) {
        const auto signature = runs.Stringify();
        if (block.Text() == text && previous == signature) return;
        previous = signature;
        block.Inlines().Clear();
        if (!runs.Size()) { block.Text(text); return; }
        for (auto value : runs) {
            auto data = value.GetObject();
            winrt::Microsoft::UI::Xaml::Documents::Run run;
            run.Text(data.GetNamedString(L"text"));
            run.FontWeight(data.GetNamedBoolean(L"matched") ? winrt::Windows::UI::Text::FontWeights::Bold()
                                                           : winrt::Windows::UI::Text::FontWeights::Normal());
            block.Inlines().Append(run);
        }
    }

    static winrt::Microsoft::UI::Xaml::Controls::TextBlock SecondaryText() {
        return winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
            LR"(<TextBlock xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis" TextWrapping="NoWrap" MaxLines="1" VerticalAlignment="Center"/>)")
            .as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
    }

    static void SizeHeadline(winrt::Microsoft::UI::Xaml::Controls::Grid const& headline) {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        const auto width = headline.ActualWidth();
        if (width <= 0) return;
        auto title = headline.Children().GetAt(0).as<TextBlock>();
        auto destination = headline.Children().GetAt(2).as<TextBlock>();
        auto action = headline.Children().GetAt(3).as<TextBlock>();
        action.MaxWidth(width * .38);
        // Measure the stock text control, including text scaling/localization.
        // Auto title + star destination keeps short titles next to their URL,
        // while bounding a long title leaves the destination readable.
        action.Measure({float(width * .38), std::numeric_limits<float>::infinity()});
        const auto remaining = std::max(0.0, width - action.DesiredSize().Width);
        title.MaxWidth(destination.Visibility() == Visibility::Visible ? remaining * .55 : remaining);
    }

    SuggestionList() {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        Automation::AutomationProperties::SetName(list, L"Address suggestions");
        Automation::AutomationProperties::SetAutomationId(list, L"FennSuggestions");
        list.MaxHeight(160);
        list.SelectionMode(ListViewSelectionMode::Single);
        // Results must appear immediately, including the first realization.
        // Override the stock ListView entrance/add/delete/reposition transitions.
        list.ItemContainerTransitions(Media::Animation::TransitionCollection{});
    }

    winrt::Microsoft::UI::Xaml::Controls::Button FirstButton() const {
        for (size_t i = 0; i < visuals.size(); ++i) if (current[i]) {
            auto const& row = visuals[i];
            for (auto const& button : row.mainButtons) {
                if (button.IsEnabled()) return button;
            }
        }
        return nullptr;
    }
    winrt::Microsoft::UI::Xaml::Documents::Hyperlink FirstHelp() const {
        for (size_t i = 0; i < visuals.size(); ++i) if (current[i]) {
            auto const& row = visuals[i];
            if (row.help) return row.help;
            for (auto const& button : row.mainButtons) if (button.IsEnabled()) return nullptr;
        }
        return nullptr;
    }
    bool FocusFirstAction() const {
        for (size_t i = 0; i < visuals.size(); ++i) if (current[i]) {
            auto const& row = visuals[i];
            if (row.help) return row.help.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard);
            for (auto const& button : row.mainButtons)
                if (button.IsEnabled()) return button.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard);
        }
        return false;
    }
    void FocusPending() {
        RestoreControlFocus();
        if (pendingFocus.empty()) return;
        for (auto const& row : visuals)
            for (uint32_t i = 0; i < row.buttonIds.size(); ++i)
                if (row.buttonIds[i] == pendingFocus) {
                    row.mainButtons[i].Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    pendingFocus = L""; return;
                }
        pendingFocus = L"";
    }
    void Clear() {
        retainedSelection = -1;
        ids.clear();
        current.clear();
        inlineUrls.clear();
        visuals.clear();
        list.Items().Clear();
    }

    void RefreshLabels() {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto container = list.Items().GetAt(i).as<ListViewItem>();
            auto title = titles[i];
            if (inlineUrls[i] && !visuals[i].url.Text().empty()) title = title + L" · " + visuals[i].url.Text();
            auto& visual = visuals[i];
            const bool showAction = (int(i) == list.SelectedIndex() || int(i) == retainedSelection) && !actionLabels[i].empty();
            visual.action.Text(actionLabels[i]);
            visual.action.Visibility(showAction ? Visibility::Visible : Visibility::Collapsed);
            if (showAction) title = title + L" · " + actionLabels[i];
            SizeHeadline(visual.headline);
            auto name = visuals[i].section.Text().empty() ? title : visuals[i].section.Text() + L". " + title;
            if (!visuals[i].description.Text().empty()) name = name + L". " + visuals[i].description.Text();
            if (!inlineUrls[i] && !visuals[i].url.Text().empty()) name = name + L". " + visuals[i].url.Text();
            if (!visuals[i].feedback.Text().empty() && visuals[i].feedback.Text() != title) name = name + L". " + visuals[i].feedback.Text();
            Automation::AutomationProperties::SetName(container, name);
        }
    }

    // Firefox assigns fresh opaque IDs as providers publish and queries change.
    // Reuse XAML slots, not those identities, to avoid container churn while
    // typing. The owner suppresses selection events
    // until SyncSelection/RefreshLabels; rebind identity-bearing callbacks too.
    void SyncRows(winrt::Windows::Data::Json::JsonArray const& rows) {
        const int previousSelection = list.SelectedIndex() >= 0 ? list.SelectedIndex() : retainedSelection;
        retainedSelection = -1;
        if (previousSelection >= 0 && size_t(previousSelection) < ids.size() && uint32_t(previousSelection) < rows.Size()) {
            auto row = rows.GetAt(previousSelection).GetObject();
            if (!row.HasKey(L"retainedSelected") && row.GetNamedBoolean(L"stale", false) && row.GetNamedString(L"id") == ids[previousSelection])
                retainedSelection = previousSelection;
        }
        // Publications can be coalesced while typing. The retained Firefox
        // selection may have an ID this list never saw; its visual authority
        // is explicit and remains separate from current activation authority.
        for (uint32_t i = 0; i < rows.Size(); ++i) {
            auto row = rows.GetAt(i).GetObject();
            if (row.GetNamedBoolean(L"stale", false) && row.GetNamedBoolean(L"retainedSelected", false)) {
                retainedSelection = int(i);
                break;
            }
        }
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        if (rows.Size() > NativeJson::kMaxListEntries) winrt::throw_hresult(E_INVALIDARG);
        // A result's presentation identity can move between slots. Preserve its
        // actual XAML objects, including focused rich controls, across that move.
        auto focused = list.XamlRoot() ? winrt::Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(list.XamlRoot()) : nullptr;
        bool ownedFocus = false;
        for (auto const& row : visuals) {
            if (row.help && focused == row.help) ownedFocus = true;
            for (auto const& button : row.mainButtons) if (focused == button) ownedFocus = true;
            for (auto const& button : row.menuButtons) if (focused == button) ownedFocus = true;
        }
        if (!ownedFocus) focused = nullptr;
        titles.clear();
        actionLabels.clear();
        current.clear();
        inlineUrls.clear();
        for (uint32_t i = 0; i < rows.Size(); ++i) {
            auto row = rows.GetAt(i).GetObject();
            auto id = row.GetNamedString(L"id");
            auto title = row.GetNamedString(L"title");
            titles.push_back(title);
            actionLabels.push_back(row.GetNamedString(L"actionLabel", L""));
            current.push_back(!row.GetNamedBoolean(L"stale", false));
            inlineUrls.push_back(row.GetNamedBoolean(L"inlineUrl", false));
            const auto wanted = row.GetNamedString(L"presentationId", id);
            size_t existing = i;
            while (existing < visuals.size() && visuals[existing].presentationId != wanted) ++existing;
            if (existing < visuals.size() && existing != i) {
                // Never detach the selected container to reorder it. Move its
                // unselected predecessors behind it instead, so the stock
                // presenter keeps its selection state throughout the update.
                if (list.SelectedItem() == list.Items().GetAt(uint32_t(existing))) {
                    for (size_t j = i; j < existing; ++j) {
                        auto preceding = list.Items().GetAt(i);
                        list.Items().RemoveAt(i);
                        list.Items().InsertAt(uint32_t(existing), preceding);
                    }
                } else {
                    auto item = list.Items().GetAt(uint32_t(existing));
                    list.Items().RemoveAt(uint32_t(existing));
                    list.Items().InsertAt(i, item);
                }
                auto visual = std::move(visuals[existing]);
                auto oldId = ids[existing];
                visuals.erase(visuals.begin() + existing); ids.erase(ids.begin() + existing);
                visuals.insert(visuals.begin() + i, std::move(visual)); ids.insert(ids.begin() + i, oldId);
            }
            bool reserved = false;
            if (existing == visuals.size() && i < visuals.size()) {
                for (uint32_t j = i + 1; j < rows.Size(); ++j) {
                    auto later = rows.GetAt(j).GetObject();
                    if (later.GetNamedString(L"presentationId", later.GetNamedString(L"id")) == visuals[i].presentationId) {
                        reserved = true; break;
                    }
                }
            }
            if (i == ids.size() || reserved) {
                ids.insert(ids.begin() + i, id);
                ListViewItem container;
                RowVisual visual;
                visual.title.Text(title);
                visual.title.TextTrimming(TextTrimming::CharacterEllipsis);
                visual.title.TextWrapping(TextWrapping::NoWrap);
                visual.title.MaxLines(1);
                visual.title.VerticalAlignment(VerticalAlignment::Center);
                visual.section = SecondaryText();
                visual.section.FontSize(12);
                visual.section.TextWrapping(TextWrapping::Wrap);
                visual.section.MaxLines(0);
                visual.section.Margin({24, 4, 0, 2});
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
                visual.destination = SecondaryText();
                visual.destination.FlowDirection(FlowDirection::LeftToRight);
                visual.separator = SecondaryText();
                visual.separator.Text(L"—");
                visual.separator.Margin({8, 0, 8, 0});
                Automation::AutomationProperties::SetAccessibilityView(visual.separator,
                                                                       Automation::Peers::AccessibilityView::Raw);
                visual.action = SecondaryText();
                visual.action.Margin({12, 0, 0, 0});
                Automation::AutomationProperties::SetAutomationId(visual.title, L"FennResultTitle");
                Automation::AutomationProperties::SetAutomationId(visual.destination, L"FennResultDestination");
                Automation::AutomationProperties::SetAutomationId(visual.action, L"FennResultAction");
                for (int column = 0; column < 4; ++column) {
                    ColumnDefinition definition;
                    definition.Width(column == 2 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star)
                                                 : GridLengthHelper::Auto());
                    visual.headline.ColumnDefinitions().Append(definition);
                }
                visual.headline.Children().Append(visual.title);
                Grid::SetColumn(visual.separator, 1);
                visual.headline.Children().Append(visual.separator);
                Grid::SetColumn(visual.destination, 2);
                visual.headline.Children().Append(visual.destination);
                Grid::SetColumn(visual.action, 3);
                visual.headline.Children().Append(visual.action);
                visual.headline.SizeChanged([](auto const& sender, auto const&) {
                    SizeHeadline(sender.template as<Grid>());
                });
                text.Children().Append(visual.headline);
                visual.description.TextWrapping(TextWrapping::Wrap);
                visual.description.MaxLines(2);
                visual.description.TextTrimming(TextTrimming::CharacterEllipsis);
                visual.url = SecondaryText();
                visual.url.FlowDirection(FlowDirection::LeftToRight);
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
                visual.symbolHost.Stretch(Media::Stretch::Uniform);
                visual.symbolHost.Child(visual.symbol);
                icon.Children().Append(visual.symbolHost);
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
                container.VerticalContentAlignment(VerticalAlignment::Center);
                container.MinHeight(36);
                container.Padding({12, 6, 12, 6});
                container.Content(content);
                container.Tag(winrt::box_value(id));
                visuals.insert(visuals.begin() + i, std::move(visual));
                list.Items().InsertAt(i, container);
            }
            ids[i] = id;
            list.Items().GetAt(i).as<ListViewItem>().Tag(winrt::box_value(id));
            auto& visual = visuals[i];
            const auto presentationId = row.GetNamedString(L"presentationId", id);
            if (visual.presentationId != presentationId) {
                visual.presentationId = presentationId;
                visual.buttons.Children().Clear();
                visual.buttonIds.clear();
                visual.mainButtons.clear();
                visual.menuButtons.clear();
                visual.bindings.clear();
                visual.helpBinding.reset();
                visual.helpKey = L"";
                visual.help = nullptr;
            }
        }
        while (ids.size() > rows.Size()) {
            list.Items().RemoveAtEnd(); ids.pop_back(); visuals.pop_back();
        }
        for (uint32_t i = 0; i < rows.Size(); ++i) {
            auto row = rows.GetAt(i).GetObject();
            auto container = list.Items().GetAt(i).as<ListViewItem>();
            // Retained rows keep their appearance while the next query runs.
            // Disable interaction separately so they do not flash disabled.
            container.IsEnabled(row.GetNamedBoolean(L"enabled", true));
            container.IsHitTestVisible(current[i]);
            container.IsTabStop(current[i]);
            auto& visual = visuals[i];
            auto buttons = row.GetNamedArray(L"buttons", winrt::Windows::Data::Json::JsonArray());
            bool sameButtons = buttons.Size() == visual.buttonIds.size();
            if (!sameButtons) {
                visual.buttons.Children().Clear(); visual.buttonIds.clear();
                visual.mainButtons.clear(); visual.menuButtons.clear();
                visual.bindings.clear();
                for (auto value : buttons) {
                    const auto id = value.GetObject().GetNamedString(L"id"), resultId = ids[i];
                    auto binding = std::make_shared<ActionBinding>();
                    visual.bindings.push_back(binding);
                    Button button; button.HorizontalAlignment(HorizontalAlignment::Left);
                    TextBlock label; label.TextWrapping(TextWrapping::Wrap); label.MaxWidth(220); button.Content(label);
                    Automation::AutomationProperties::SetAutomationId(button, L"FennTip-" + id);
                    button.Click([this, binding = std::weak_ptr<ActionBinding>(binding), alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (auto action = binding.lock(); !alive.expired() && action && action->enabled && activateButton)
                            activateButton(action->actionId, action->resultId);
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
                    Automation::AutomationProperties::SetAutomationId(menuButton, L"FennTipMenu-" + id);
                    menuButton.Click([this, binding = std::weak_ptr<ActionBinding>(binding), alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto const&) {
                        if (auto action = binding.lock(); !alive.expired() && action && action->enabled && openButtonMenu)
                            openButtonMenu(action->actionId, action->resultId, sender.template as<FrameworkElement>());
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
                auto& binding = *visual.bindings[b];
                binding = {ids[i], data.GetNamedString(L"id"), current[i] && data.GetNamedBoolean(L"enabled")};
                visual.buttonIds[b] = binding.actionId;
                Automation::AutomationProperties::SetAutomationId(button, L"FennTip-" + binding.actionId);
                Automation::AutomationProperties::SetAutomationId(menuButton, L"FennTipMenu-" + binding.actionId);
                menuButton.Visibility(data.GetNamedBoolean(L"menu", false) ? Visibility::Visible : Visibility::Collapsed);
                menuButton.IsEnabled(data.GetNamedBoolean(L"enabled"));
                menuButton.IsTabStop(current[i]);
                Automation::AutomationProperties::SetName(menuButton, data.GetNamedString(L"menuLabel", L""));
                auto label = data.GetNamedString(L"label");
                button.Content().as<TextBlock>().Text(label);
                button.IsEnabled(data.GetNamedBoolean(L"enabled"));
                button.IsTabStop(current[i]);
                Automation::AutomationProperties::SetName(button, label);
            }
            if (buttons.Size() && row.GetNamedBoolean(L"focusButton", false) && visual.buttonIds[0] != lastFocus) {
                pendingFocus = lastFocus = visual.buttonIds[0];
            }
            visual.buttons.Visibility(buttons.Size() ? Visibility::Visible : Visibility::Collapsed);
            const auto feedback = row.GetNamedString(L"feedback", L"");
            const bool acknowledgment = !feedback.empty() && feedback == row.GetNamedString(L"title");
            Automation::AutomationProperties::SetLiveSetting(visual.title, acknowledgment
                ? Automation::Peers::AutomationLiveSetting::Polite : Automation::Peers::AutomationLiveSetting::Off);
            if (visual.feedback.Text() != feedback) {
                visual.feedback.Text(feedback);
                auto target = acknowledgment ? visual.title : visual.feedback;
                if (acknowledgment) { target.Text(feedback); visual.titleRunsKey = L""; }
                Automation::AutomationProperties::SetLiveSetting(target, Automation::Peers::AutomationLiveSetting::Polite);
                if (auto peer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(target))
                    peer.RaiseAutomationEvent(Automation::Peers::AutomationEvents::LiveRegionChanged);
            }
            visual.feedback.Visibility(feedback.empty() || feedback == row.GetNamedString(L"title") ? Visibility::Collapsed : Visibility::Visible);
            const auto description = row.GetNamedString(L"description", L"");
            const auto url = row.GetNamedString(L"displayUrl", L"");
            if (row.HasKey(L"helpLink")) {
                auto help = row.GetNamedObject(L"helpLink");
                auto presentation = JObject::Parse(help.Stringify());
                presentation.Remove(L"id");
                auto key = presentation.Stringify();
                if (key != visual.helpKey) {
                    visual.description.Inlines().Clear();
                    Documents::Run before, label, after;
                    before.Text(help.GetNamedString(L"before")); label.Text(help.GetNamedString(L"label")); after.Text(help.GetNamedString(L"after"));
                    visual.help = Documents::Hyperlink();
                    visual.helpBinding = std::make_shared<ActionBinding>();
                    visual.help.Inlines().Append(label);
                    const auto id = help.GetNamedString(L"id"), resultId = ids[i];
                    Automation::AutomationProperties::SetAutomationId(visual.help, L"FennHelp-" + id);
                    Automation::AutomationProperties::SetName(visual.help, help.GetNamedString(L"label"));
                    visual.help.Click([this, binding = std::weak_ptr<ActionBinding>(visual.helpBinding), alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (auto action = binding.lock(); !alive.expired() && action && action->enabled && activateButton)
                            activateButton(action->actionId, action->resultId);
                    });
                    visual.description.Inlines().Append(before); visual.description.Inlines().Append(visual.help); visual.description.Inlines().Append(after);
                    visual.helpKey = key;
                }
                *visual.helpBinding = {ids[i], help.GetNamedString(L"id"), current[i]};
                visual.help.IsTabStop(current[i]);
                Automation::AutomationProperties::SetAutomationId(visual.help, L"FennHelp-" + visual.helpBinding->actionId);
                visual.description.MaxLines(0); // Keep the interactive link reachable when text wraps.
            } else {
                visual.helpBinding.reset();
                visual.help = nullptr; visual.helpKey = L"";
                visual.description.Text(description); visual.description.MaxLines(2);
            }
            visual.description.Visibility(description.empty() ? Visibility::Collapsed : Visibility::Visible);
            RenderText(visual.title, titles[i], row.GetNamedArray(L"titleRuns", winrt::Windows::Data::Json::JsonArray()), visual.titleRunsKey);
            const auto shownUrl = url == titles[i] ? winrt::hstring{} : url;
            const auto urlRuns = shownUrl.empty() ? winrt::Windows::Data::Json::JsonArray()
                : row.GetNamedArray(L"urlRuns", winrt::Windows::Data::Json::JsonArray());
            RenderText(visual.url, shownUrl, urlRuns, visual.urlRunsKey);
            visual.url.Visibility(visual.url.Text().empty() || inlineUrls[i] ? Visibility::Collapsed : Visibility::Visible);
            RenderText(visual.destination, shownUrl, urlRuns, visual.destinationRunsKey);
            const auto inlineVisibility = !visual.url.Text().empty() && inlineUrls[i] ? Visibility::Visible : Visibility::Collapsed;
            visual.destination.Visibility(inlineVisibility);
            visual.separator.Visibility(inlineVisibility);
            ToolTipService::SetToolTip(visual.title, winrt::box_value(titles[i]));
            ToolTipService::SetToolTip(visual.destination, winrt::box_value(url));
            ToolTipService::SetToolTip(visual.url, winrt::box_value(url));
            ToolTipService::SetToolTip(visual.action, winrt::box_value(actionLabels[i]));
            visual.title.FlowDirection(row.GetNamedBoolean(L"titleLtr", false)
                                           ? FlowDirection::LeftToRight : list.FlowDirection());
            const auto section = row.GetNamedString(L"sectionLabel", L"");
            visual.section.Text(section);
            visual.section.Visibility(section.empty() ? Visibility::Collapsed : Visibility::Visible);
            const auto size = row.GetNamedNumber(L"imageSize", 16);
            visual.image.Width(size);
            visual.image.Height(size);
            // SymbolIcon has a stock glyph size; Width/Height alone clip it.
            // Let WinUI scale its natural layout into Firefox's icon slot.
            visual.symbolHost.Width(size);
            visual.symbolHost.Height(size);
            const auto symbol = row.GetNamedString(L"symbol", L"page");
            visual.symbol.Symbol(symbol == L"search"    ? Symbol::Find
                                 : symbol == L"history" ? Symbol::Clock
                                 : symbol == L"copy"    ? Symbol::Copy
                                 : symbol == L"bookmarks" ? Symbol::Favorite
                                 : symbol == L"tabs"      ? Symbol::BrowsePhotos
                                 : symbol == L"actions"   ? Symbol::AllApps
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
            visual.symbolHost.Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
        }
        pendingControlFocus = focused;
    }

    void RestoreControlFocus() {
        if (!pendingControlFocus) return;
        // Removing/reinserting a container can drop XAML focus. Restore only an
        // exact surviving control, after its callback has the current identity.
        list.UpdateLayout();
        const auto focused = pendingControlFocus;
        pendingControlFocus = nullptr;
        if (focused) for (size_t i = 0; i < visuals.size(); ++i) {
            if (!current[i]) continue;
            auto& row = visuals[i];
            if (row.help && focused == row.help && row.help.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard)) return;
            for (auto const& button : row.mainButtons)
                if (focused == button && button.IsEnabled() && button.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard)) return;
            for (auto const& button : row.menuButtons)
                if (focused == button && button.IsEnabled() && button.Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible &&
                    button.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard)) return;
        }
        // Only a control owned by this list was captured. If it disappeared,
        // became stale or disabled, restore the edit target, not a nearby row.
        if (focusField) focusField();
    }

    void SyncSelection(winrt::hstring const& selectedId) {
        auto selectedIt = std::find(ids.begin(), ids.end(), selectedId);
        const int selected = selectedIt == ids.end() || !current[selectedIt - ids.begin()]
                                 ? -1 : int(selectedIt - ids.begin());
        if (selected >= 0) retainedSelection = -1;
        const int displayed = selected >= 0 ? selected : retainedSelection;
        // Do not deselect/reselect the stock presenter between queries: its
        // selection animation survives a subsequent GoToState(false). Retained
        // selection is historical presentation, never a current command target.
        if (list.SelectedIndex() != displayed) list.SelectedIndex(displayed);
    }

    int CurrentSelectedIndex() const {
        const int index = list.SelectedIndex();
        return index >= 0 && size_t(index) < current.size() && current[index] ? index : -1;
    }

    bool HasCurrentRows() const {
        return std::find(current.begin(), current.end(), true) != current.end();
    }

    // The ids whose rows are actually within the list's viewport. Returns them
    // rather than appending to a JSON array: the caller compares this against
    // what it last reported, and building JSON to answer "did anything change"
    // is work done to discover that nothing has.
    std::vector<winrt::hstring> VisibleIds() const {
        using namespace winrt::Microsoft::UI::Xaml;
        std::vector<winrt::hstring> visible;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            if (!current[i]) continue;
            auto row = list.ContainerFromIndex(i).try_as<FrameworkElement>();
            if (!row || row.ActualHeight() <= 0) continue;
            auto origin = row.TransformToVisual(list).TransformPoint({0, 0});
            if (origin.Y < list.ActualHeight() && origin.Y + row.ActualHeight() > 0)
                visible.push_back(ids[i]);
        }
        return visible;
    }
};
