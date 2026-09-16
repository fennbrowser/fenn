#pragma once
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "JsonValidation.h"

struct NativeAddress {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using JValue = winrt::Windows::Data::Json::JsonValue;
    // Icon-sized images (mode button, mode menu entries, global actions) vs.
    // the larger favicons address rows can carry. Two distinct budgets, not
    // one duplicated by coincidence: keep them named instead of re-deriving
    // the same four literals at every call site.
    static constexpr NativeJson::ImageLimits kIconImageLimits{32, 8192};
    static constexpr NativeJson::ImageLimits kRowImageLimits{104, 65536};
    Microsoft::UI::Xaml::Controls::StackPanel root;
    Microsoft::UI::Xaml::Controls::TextBox field;
    Microsoft::UI::Xaml::Controls::DropDownButton modeButton;
    Microsoft::UI::Xaml::Controls::MenuFlyout modeFlyout;
    Microsoft::UI::Xaml::Controls::Button exitMode;
    Microsoft::UI::Xaml::Controls::Image modeImage;
    Microsoft::UI::Xaml::Controls::SymbolIcon modeSymbol;
    Microsoft::UI::Xaml::Controls::TextBlock modeLabel;
    std::vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItem> modeItems;
    std::vector<winrt::hstring> modeIds, modePixels;
    winrt::hstring modePending, buttonPixels;
    double modeGeneration{-1}, modeApplied{}, modeReveal{};
    bool revealModePending{};
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
    Microsoft::UI::Xaml::Controls::StackPanel globalPanel;
    struct GlobalVisual {
        Microsoft::UI::Xaml::Controls::Button button;
        Microsoft::UI::Xaml::Controls::Image image;
        Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
        Microsoft::UI::Xaml::Controls::TextBlock text, hint;
        winrt::hstring id, resultId, pixels;
    };
    std::vector<GlobalVisual> globalVisuals;
    winrt::hstring session, pending, visibility, observedText, submitIntent;
    double revision{}, sequence{-1}, editId{}, selectionId{}, acknowledgedEditId{};
    bool syncing{}, composing{}, enterPending{}, observedComposing{}, queryComplete{}, focusWithin{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~NativeAddress() { lifetime.reset(); if (modeFlyout.IsOpen()) modeFlyout.Hide(); }

    static void ValidateModeImage(JObject const& item, size_t& total) {
        NativeJson::ValidateImage(item, total, kIconImageLimits);
    }
    static Microsoft::UI::Xaml::Media::ImageSource ModeImage(JObject const& item) {
        return NativeJson::DecodeImage(item.GetNamedObject(L"image"));
    }
    static Microsoft::UI::Xaml::Controls::Symbol ModeSymbol(winrt::hstring const& symbol) {
        using Microsoft::UI::Xaml::Controls::Symbol;
        if (symbol == L"bookmarks") return Symbol::Favorite;
        if (symbol == L"tabs") return Symbol::BrowsePhotos;
        if (symbol == L"history") return Symbol::Clock;
        if (symbol == L"actions") return Symbol::AllApps;
        if (symbol == L"settings") return Symbol::Setting;
        if (symbol == L"add") return Symbol::Add;
        return Symbol::Find;
    }
    void QueueMode(wchar_t const* kind, winrt::hstring const& id = L"") {
        if (syncing || composing || session.empty()) return;
        Edit();
        auto value = Message(kind);
        value.SetNamedValue(L"generation", JValue::CreateNumberValue(modeGeneration));
        value.SetNamedValue(L"id", JValue::CreateStringValue(id));
        JObject intent;
        intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL)&0x8000)!=0));
        intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU)&0x8000)!=0));
        intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT)&0x8000)!=0));
        intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue(false));
        value.SetNamedValue(L"intent", intent);
        modePending = value.Stringify();
        enterPending = false; submitIntent = L"";
    }
    void NewModeFlyout() {
        using namespace Microsoft::UI::Xaml;
        if (modeFlyout.IsOpen()) modeFlyout.Hide();
        modeFlyout = Controls::MenuFlyout();
        modeButton.Flyout(modeFlyout);
        modeFlyout.Opening([this, alive=std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (!alive.expired()) QueueMode(L"mode-open"); });
        modeFlyout.Opened([this, alive=std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired() && !modeItems.empty()) modeItems.front().Focus(FocusState::Programmatic);
        });
    }
    void SyncModes(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const bool visible=snapshot.GetNamedBoolean(L"modeButtonVisible",true);
        if (!visible) {
            if (modeFlyout.IsOpen()) modeFlyout.Hide();
            if (modeButton.FocusState()!=FocusState::Unfocused || exitMode.FocusState()!=FocusState::Unfocused)
                field.Focus(FocusState::Programmatic);
        }
        modeButton.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (!visible) revealModePending=false;
        const auto reveal=snapshot.GetNamedNumber(L"modeReveal",0);
        if (reveal!=modeReveal) {modeReveal=reveal; revealModePending=visible;}
        if (!snapshot.HasKey(L"modes") || snapshot.GetNamedValue(L"modes").ValueType() == winrt::Windows::Data::Json::JsonValueType::Null) {
            modeButton.IsEnabled(false);
            exitMode.Visibility(Visibility::Collapsed);
            if (modeFlyout.IsOpen()) modeFlyout.Hide();
            return;
        }
        auto modes = snapshot.GetNamedObject(L"modes");
        auto entries = modes.GetNamedArray(L"entries");
        auto generation = modes.GetNamedNumber(L"generation");
        if (modeGeneration != generation) {
            NewModeFlyout();
            modeItems.clear(); modeIds.clear(); modePixels.clear();
            modeGeneration = generation;
            bool local = false, offered = false;
            for (auto value : entries) {
                auto entry = value.GetObject();
                auto symbol=entry.GetNamedString(L"symbol",L"");
                if ((!local && entry.GetNamedBoolean(L"local", false)) || (symbol==L"add" && !offered) || (symbol==L"settings" && !offered)) {
                    modeFlyout.Items().Append(MenuFlyoutSeparator()); local = true;
                }
                offered = offered || symbol==L"add";
                MenuFlyoutItem item; item.Text(entry.GetNamedString(L"label"));
                auto id = entry.GetNamedString(L"id");
                item.Click([this, alive=std::weak_ptr<int>(lifetime), id, generation](auto const&, auto const&) {
                    if (!alive.expired() && modeGeneration == generation) QueueMode(L"mode-pick", id);
                });
                Automation::AutomationProperties::SetAutomationId(item, id);
                modeFlyout.Items().Append(item); modeItems.push_back(item); modeIds.push_back(id); modePixels.push_back(L"");
            }
        }
        for (uint32_t i=0; i<entries.Size(); ++i) {
            auto entry = entries.GetAt(i).GetObject();
            auto encoded = entry.GetNamedValue(L"image", JValue::CreateNullValue()).Stringify();
            if (modePixels[i] != encoded) {
                if (encoded != L"null") { ImageIcon icon; icon.Source(ModeImage(entry)); modeItems[i].Icon(icon); }
                else { SymbolIcon icon; icon.Symbol(ModeSymbol(entry.GetNamedString(L"symbol", L"search"))); modeItems[i].Icon(icon); }
                modePixels[i] = encoded;
            }
        }
        auto encoded = modes.GetNamedValue(L"image", JValue::CreateNullValue()).Stringify();
        if (buttonPixels != encoded) { modeImage.Source(encoded == L"null" ? nullptr : ModeImage(modes)); buttonPixels = encoded; }
        modeImage.Visibility(encoded == L"null" ? Visibility::Collapsed : Visibility::Visible);
        modeSymbol.Visibility(encoded == L"null" ? Visibility::Visible : Visibility::Collapsed);
        modeSymbol.Symbol(ModeSymbol(modes.GetNamedString(L"symbol", L"search")));
        auto label = modes.GetNamedString(L"label");
        Automation::AutomationProperties::SetName(modeButton, modes.GetNamedString(L"buttonLabel", label));
        Automation::AutomationProperties::SetName(exitMode, modes.GetNamedString(L"exitLabel", L"Exit search mode"));
        modeLabel.Text(modes.GetNamedBoolean(L"active") ? label : L"");
        modeLabel.MaxWidth(80); modeLabel.TextTrimming(TextTrimming::CharacterEllipsis);
        exitMode.Visibility(visible && snapshot.GetNamedBoolean(L"modeExitVisible",modes.GetNamedBoolean(L"active")) ? Visibility::Visible : Visibility::Collapsed);
        modeButton.IsEnabled(true);
        const double applied = modes.GetNamedNumber(L"applied", 0);
        if (modeApplied != applied) { modeApplied = applied; field.Focus(FocusState::Programmatic); }
    }
    void SyncGlobal(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        auto actions=snapshot.GetNamedArray(L"globalActions", winrt::Windows::Data::Json::JsonArray());
        bool same=actions.Size()==globalVisuals.size();
        for (uint32_t i=0; same && i<actions.Size(); ++i) same=actions.GetAt(i).GetObject().GetNamedString(L"id")==globalVisuals[i].id;
        if (!same) {globalVisuals.clear(); globalPanel.Children().Clear();}
        for (uint32_t i=0; i<actions.Size(); ++i) {
            auto item=actions.GetAt(i).GetObject();
            if (!same) {
                GlobalVisual visual; visual.id=item.GetNamedString(L"id"); visual.resultId=item.GetNamedString(L"resultId");
                StackPanel group; group.Spacing(2);
                StackPanel content; content.Orientation(Orientation::Horizontal); content.Spacing(6);
                Grid icon; icon.Children().Append(visual.symbol); icon.Children().Append(visual.image);
                visual.image.Width(16); visual.image.Height(16); visual.image.Stretch(Media::Stretch::Uniform);
                visual.symbol.Symbol(Symbol::Find); visual.symbol.Width(16); visual.symbol.Height(16);
                Automation::AutomationProperties::SetAccessibilityView(visual.image, Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(visual.symbol, Automation::Peers::AccessibilityView::Raw);
                visual.text.TextWrapping(TextWrapping::Wrap); visual.text.MaxWidth(180);
                content.Children().Append(icon); content.Children().Append(visual.text);
                visual.button.Content(content); visual.button.HorizontalAlignment(HorizontalAlignment::Left);
                Automation::AutomationProperties::SetAutomationId(visual.button, visual.id);
                auto id=visual.id, resultId=visual.resultId;
                visual.button.Click([this, alive=std::weak_ptr<int>(lifetime), id, resultId](auto const&, auto const&) {
                    if (alive.expired() || syncing || composing) return;
                    Edit();
                    if (acknowledgedEditId!=editId || (!pending.empty() && JObject::Parse(pending).GetNamedString(L"kind")!=L"text-selection")) return;
                    auto value=Message(L"global-action"); value.SetNamedValue(L"id",JValue::CreateStringValue(resultId));
                    value.SetNamedValue(L"actionId",JValue::CreateStringValue(id));
                    JObject intent;
                    intent.SetNamedValue(L"ctrlKey",JValue::CreateBooleanValue((GetKeyState(VK_CONTROL)&0x8000)!=0));
                    intent.SetNamedValue(L"shiftKey",JValue::CreateBooleanValue((GetKeyState(VK_SHIFT)&0x8000)!=0));
                    intent.SetNamedValue(L"altKey",JValue::CreateBooleanValue((GetKeyState(VK_MENU)&0x8000)!=0));
                    value.SetNamedValue(L"intent",intent); pending=value.Stringify(); enterPending=false;
                });
                visual.button.PreviewKeyDown([this, alive=std::weak_ptr<int>(lifetime)](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
                    if (alive.expired()) return;
                    using winrt::Windows::System::VirtualKey;
                    if (e.Key()==VirtualKey::Escape) {Action(L"dismiss"); field.Focus(FocusState::Keyboard); e.Handled(true);}
                    if (e.Key()==VirtualKey::Tab && (GetKeyState(VK_SHIFT)&0x8000) &&
                        Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot()) == globalVisuals.front().button) {
                        field.Focus(FocusState::Keyboard); e.Handled(true);
                    }
                });
                group.Children().Append(visual.hint); group.Children().Append(visual.button);
                globalPanel.Children().Append(group); globalVisuals.push_back(visual);
            }
            auto& visual=globalVisuals[i];
            visual.text.Text(item.GetNamedString(L"title"));
            Automation::AutomationProperties::SetName(visual.button,visual.text.Text());
            auto hint=item.GetNamedString(L"hint",L""); visual.hint.Text(hint);
            visual.hint.Visibility(hint.empty()?Visibility::Collapsed:Visibility::Visible);
            auto pixels=item.GetNamedValue(L"image",JValue::CreateNullValue()).Stringify();
            if (pixels!=visual.pixels) {visual.image.Source(pixels==L"null"?nullptr:ModeImage(item)); visual.pixels=pixels;}
            visual.image.Visibility(pixels==L"null"?Visibility::Collapsed:Visibility::Visible);
            visual.symbol.Visibility(pixels==L"null"?Visibility::Visible:Visibility::Collapsed);
        }
        globalPanel.Visibility(actions.Size()?Visibility::Visible:Visibility::Collapsed);
    }
    JObject Message(wchar_t const* kind) {
        JObject value;
        value.SetNamedValue(L"version", JValue::CreateNumberValue(1));
        value.SetNamedValue(L"session", JValue::CreateStringValue(session));
        value.SetNamedValue(L"revision", JValue::CreateNumberValue(revision));
        value.SetNamedValue(L"kind", JValue::CreateStringValue(kind));
        return value;
    }
    void Edit() {
        if (syncing || session.empty()) return;
        if (field.Text() == observedText && composing == observedComposing) return;
        observedText = field.Text(); observedComposing = composing;
        enterPending = false; submitIntent = L"";
        auto value = Message(L"edit");
        value.SetNamedValue(L"editId", JValue::CreateNumberValue(++editId));
        value.SetNamedValue(L"text", JValue::CreateStringValue(field.Text()));
        value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
        value.SetNamedValue(L"selectionEnd", JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
        value.SetNamedValue(L"composing", JValue::CreateBooleanValue(composing));
        pending = value.Stringify();
    }
    void Action(wchar_t const* kind) {
        if (syncing || composing || session.empty() || !modePending.empty()) return;
        if (wcscmp(kind, L"activate") == 0) {
            Edit();
            if (submitIntent.empty()) {
                JObject intent;
                intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
                intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU) & 0x8000) != 0));
                intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
                intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue((GetKeyState(VK_LWIN) & 0x8000) != 0));
                submitIntent = intent.Stringify();
            }
            // An edit can already have left the queue while Firefox is still
            // querying or has not acknowledged it in a native snapshot yet.
            if (acknowledgedEditId != editId || (!queryComplete && ids.empty())) { enterPending = true; return; }
        }
        if (!pending.empty()) {
            auto kindPending = JObject::Parse(pending).GetNamedString(L"kind");
            if (wcscmp(kind, L"activate") == 0 && kindPending == L"edit") { enterPending = true; return; }
            const bool replaceSelection = kindPending == L"text-selection" &&
                (wcscmp(kind, L"select") == 0 || wcscmp(kind, L"activate") == 0);
            const bool activateSelected = kindPending == L"select" && wcscmp(kind, L"activate") == 0;
            const bool restoreFocus = wcscmp(kind, L"focus") == 0 &&
                (kindPending == L"text-selection" || kindPending == L"blur");
            if (wcscmp(kind, L"dismiss") != 0 && wcscmp(kind, L"blur") != 0 && !replaceSelection && !activateSelected && !restoreFocus) return;
        }
        if (wcscmp(kind, L"dismiss") == 0 || wcscmp(kind, L"blur") == 0) { enterPending = false; submitIntent = L""; }
        auto value = Message(kind);
        if (wcscmp(kind, L"activate") == 0) {
            value.SetNamedValue(L"intent", JObject::Parse(submitIntent));
            submitIntent = L"";
        }
        int index = list.SelectedIndex();
        if (index >= 0 && size_t(index) < ids.size()) value.SetNamedValue(L"id", JValue::CreateStringValue(ids[index]));
        pending = value.Stringify();
    }
    void RefreshLabels() {
        using namespace Microsoft::UI::Xaml::Controls;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto container = list.Items().GetAt(i).as<ListViewItem>();
            auto title = titles[i];
            if (int(i) == list.SelectedIndex() && !actionLabels[i].empty())
                title = title + L" \u00b7 " + actionLabels[i];
            visuals[i].title.Text(title);
            Automation::AutomationProperties::SetName(container, visuals[i].section.Text().empty() ? title : visuals[i].section.Text() + L". " + title);
        }
    }
    void ObserveFocus(bool islandHasFocus) {
        if (modeFlyout.IsOpen()) return;
        bool inside = false;
        if (root.XamlRoot() && islandHasFocus) {
            auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<DependencyObject>();
            while (focused) {
                if (focused == root) { inside = true; break; }
                focused = Media::VisualTreeHelper::GetParent(focused);
            }
        }
        // XAML retains logical focus when Win32 focus leaves the island.
        if (inside != focusWithin) {
            focusWithin = inside;
            Action(inside ? L"focus" : L"blur");
            if (GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0))
                fwprintf(stderr, L"Native address focus inside=%d island=%d pending=%s\n", inside, islandHasFocus, pending.empty() ? L"none" : JObject::Parse(pending).GetNamedString(L"kind").c_str());
        }
    }
    NativeAddress() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        root.Visibility(Visibility::Collapsed);
        root.Spacing(4);
        root.Margin({0,0,0,12});
        field.PlaceholderText(L"Search or enter address");
        field.MaxLength(32768);
        Automation::AutomationProperties::SetName(field, L"Native address prototype");
        Automation::AutomationProperties::SetAutomationId(field, L"FirefoxWinUIAddress");
        Automation::AutomationProperties::SetName(list, L"Address suggestions");
        Automation::AutomationProperties::SetAutomationId(list, L"FirefoxWinUISuggestions");
        list.MaxHeight(160);
        list.SelectionMode(ListViewSelectionMode::Single);
        field.TextChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (alive.expired()) return; Edit(); });
        field.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || syncing || composing || session.empty()) return;
            if (!pending.empty()) {
                auto value = JObject::Parse(pending);
                if (value.GetNamedString(L"kind") == L"edit") {
                    value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
                    value.SetNamedValue(L"selectionEnd", JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
                    pending = value.Stringify();
                }
                return;
            }
            auto value = Message(L"text-selection");
            value.SetNamedValue(L"selectionId", JValue::CreateNumberValue(++selectionId));
            value.SetNamedValue(L"start", JValue::CreateNumberValue(field.SelectionStart()));
            value.SetNamedValue(L"end", JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
            pending = value.Stringify();
        });
        field.TextCompositionStarted([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (alive.expired()) return; composing = true; Edit(); });
        field.TextCompositionEnded([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (alive.expired()) return; composing = false; Edit(); });
        // TextBox handles pointer presses internally; observe without handling
        // them again so native caret placement and selection remain standard.
        field.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) Action(L"focus");
            })), true);
        field.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) Action(L"focus");
        });
        field.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (alive.expired()) return;
            if (composing) return;
            using winrt::Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Enter) { Action(L"activate"); e.Handled(true); }
            if (e.Key() == VirtualKey::Escape) { Action(L"dismiss"); e.Handled(true); }
            if (e.Key() == VirtualKey::Down && !ids.empty() && !(GetKeyState(VK_MENU)&0x8000)) {
                int next = list.SelectedIndex() + 1;
                while (next < int(ids.size()) && !list.Items().GetAt(next).as<ListViewItem>().IsEnabled()) ++next;
                if (next >= int(ids.size())) { e.Handled(true); return; }
                syncing = true; list.Focus(FocusState::Keyboard); list.SelectedIndex(next); syncing = false;
                RefreshLabels(); Action(L"select"); e.Handled(true);
            }
        });
        list.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (alive.expired()) return; if (!syncing) RefreshLabels(); Action(L"select"); });
        list.IsItemClickEnabled(true);
        list.ItemClick([this, alive = std::weak_ptr<int>(lifetime)](auto const&, ItemClickEventArgs const& e) {
            if (alive.expired()) return;
            uint32_t index{};
            auto clicked = e.ClickedItem();
            bool found = list.Items().IndexOf(clicked, index);
            // Explicit ListViewItem entries report their Content as ClickedItem.
            // Match that exact object; never infer the clicked row from selection.
            if (!found) {
                for (uint32_t i = 0; i < list.Items().Size(); ++i) {
                    auto container = list.Items().GetAt(i).try_as<ListViewItem>();
                    if (container && container.Content() == clicked) { index = i; found = true; break; }
                }
            }
            if (found) {
                syncing = true; list.SelectedIndex(int(index)); syncing = false;
                Action(L"activate");
            }
        });
        list.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (alive.expired()) return;
            using winrt::Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Enter) { Action(L"activate"); e.Handled(true); }
            if (e.Key() == VirtualKey::Escape) { Action(L"dismiss"); e.Handled(true); field.Focus(FocusState::Keyboard); }
        });
        Button fallback;
        fallback.Content(winrt::box_value(L"Use Firefox address bar"));
        fallback.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (alive.expired()) return; Action(L"fallback"); });
        Grid header;
        ColumnDefinition buttonColumn; buttonColumn.Width(GridLengthHelper::Auto());
        ColumnDefinition fieldColumn; fieldColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition exitColumn; exitColumn.Width(GridLengthHelper::Auto());
        header.ColumnDefinitions().Append(buttonColumn); header.ColumnDefinitions().Append(fieldColumn); header.ColumnDefinitions().Append(exitColumn);
        Grid::SetColumn(field, 1); Grid::SetColumn(exitMode, 2);
        NewModeFlyout(); modeButton.IsEnabled(false); modeButton.Padding({4,0,4,0});
        Automation::AutomationProperties::SetAutomationId(modeButton, L"FirefoxWinUISearchMode");
        StackPanel buttonContent; buttonContent.Orientation(Orientation::Horizontal); buttonContent.Spacing(4);
        Automation::AutomationProperties::SetAccessibilityView(modeImage, Automation::Peers::AccessibilityView::Raw);
        Automation::AutomationProperties::SetAccessibilityView(modeSymbol, Automation::Peers::AccessibilityView::Raw);
        modeImage.Width(16); modeImage.Height(16); modeImage.Stretch(Media::Stretch::Uniform);
        modeSymbol.Symbol(Symbol::Find);
        buttonContent.Children().Append(modeImage); buttonContent.Children().Append(modeSymbol); buttonContent.Children().Append(modeLabel);
        modeButton.Content(buttonContent);
        exitMode.Content(winrt::box_value(L"×")); exitMode.Padding({4,0,4,0}); exitMode.MinWidth(24);
        Automation::AutomationProperties::SetName(exitMode, L"Exit search mode");
        Automation::AutomationProperties::SetAutomationId(exitMode, L"FirefoxWinUIExitSearchMode");
        exitMode.Visibility(Visibility::Collapsed);
        exitMode.Click([this, alive=std::weak_ptr<int>(lifetime)](auto const&, auto const&) { if (!alive.expired()) QueueMode(L"mode-exit"); });
        field.PreviewKeyDown([this, alive=std::weak_ptr<int>(lifetime)](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!alive.expired() && (GetKeyState(VK_MENU)&0x8000) &&
                (e.Key()==winrt::Windows::System::VirtualKey::Down || e.Key()==winrt::Windows::System::VirtualKey::Up)) {
                QueueMode(L"mode-reveal"); e.Handled(true);
            }
        });
        header.Children().Append(modeButton); header.Children().Append(field); header.Children().Append(exitMode);
        globalPanel.Spacing(4); globalPanel.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetName(globalPanel,L"Search actions");
        field.PreviewKeyDown([this, alive=std::weak_ptr<int>(lifetime)](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!alive.expired() && !composing && e.Key()==winrt::Windows::System::VirtualKey::Tab &&
                !(GetKeyState(VK_SHIFT)&0x8000) && !globalVisuals.empty()) {
                globalVisuals.front().button.Focus(FocusState::Keyboard); e.Handled(true);
            }
        });
        root.Children().Append(header); root.Children().Append(list); root.Children().Append(globalPanel); root.Children().Append(fallback);
    }
    winrt::hstring Sync(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (snapshot.GetNamedNumber(L"version") != 1) winrt::throw_hresult(E_INVALIDARG);
        if (!snapshot.GetNamedBoolean(L"enabled", false)) {
            if (modeFlyout.IsOpen()) modeFlyout.Hide(); modePending = L"";
            globalVisuals.clear(); globalPanel.Children().Clear();
            syncing = true; root.Visibility(Visibility::Collapsed); list.Items().Clear();
            ids.clear(); visuals.clear(); pending = L""; session = L""; enterPending = false; submitIntent = L""; queryComplete = false; acknowledgedEditId = 0; syncing = false; return L"";
        }
        auto nextSession = snapshot.GetNamedString(L"session");
        NativeJson::RequireRevision(snapshot, L"revision");
        auto checkedSequence = snapshot.GetNamedNumber(L"sequence", 0);
        auto checkedText = snapshot.GetNamedString(L"text", L"");
        auto checkedRows = snapshot.GetNamedArray(L"rows");
        if (nextSession.empty() || nextSession.size() > 256 ||
            checkedText.size() > 32768 || checkedRows.Size() > 100)
            winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> checkedIds;
        size_t imagePixels = 0;
        for (auto value : checkedRows) {
            auto row = value.GetObject();
            auto id = row.GetNamedString(L"id");
            if (id.empty() || id.size() > 256 || !checkedIds.insert(id).second || row.GetNamedString(L"title").size() > 4096 || row.GetNamedString(L"actionLabel", L"").size() > 1024)
                winrt::throw_hresult(E_INVALIDARG);
            const auto size = row.GetNamedNumber(L"imageSize", 16);
            if (size < 16 || size > 52 || row.GetNamedString(L"sectionLabel", L"").size() > 1024)
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::ValidateImage(row, imagePixels, kRowImageLimits);
        }
        auto globalActions=snapshot.GetNamedArray(L"globalActions",winrt::Windows::Data::Json::JsonArray());
        if (globalActions.Size()>16) winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> actionIds;
        for (auto value:globalActions) {
            auto item=value.GetObject(); auto id=item.GetNamedString(L"id"); auto resultId=item.GetNamedString(L"resultId");
            if (id.empty() || id.size()>256 || !actionIds.insert(id).second || resultId.empty() || resultId.size()>256 ||
                item.GetNamedString(L"title").size()>1024 || item.GetNamedString(L"hint",L"").size()>1024) winrt::throw_hresult(E_INVALIDARG);
            size_t pixels=0; ValidateModeImage(item,pixels);
        }
        if (snapshot.HasKey(L"modes") && snapshot.GetNamedValue(L"modes").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) {
            auto modes = snapshot.GetNamedObject(L"modes"); auto entries = modes.GetNamedArray(L"entries");
            if (entries.Size() > 100 || modes.GetNamedString(L"label").size() > 1024 || modes.GetNamedString(L"buttonLabel", L"").size()>1024 || modes.GetNamedString(L"exitLabel", L"").size()>1024) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireRevision(modes, L"generation");
            if (nextSession == session && modes.GetNamedNumber(L"generation") == modeGeneration) {
                if (entries.Size() != modeIds.size()) winrt::throw_hresult(E_INVALIDARG);
                for (uint32_t i=0; i<entries.Size(); ++i)
                    if (entries.GetAt(i).GetObject().GetNamedString(L"id") != modeIds[i]) winrt::throw_hresult(E_INVALIDARG);
            }
            size_t pixels=0; ValidateModeImage(modes, pixels); std::set<winrt::hstring> unique;
            for (auto item : entries) { auto entry=item.GetObject(); auto id=entry.GetNamedString(L"id");
                if (id.empty() || id.size()>256 || !unique.insert(id).second || entry.GetNamedString(L"label").size()>1024) winrt::throw_hresult(E_INVALIDARG);
                ValidateModeImage(entry, pixels);
            }
        }
        if (nextSession != session) {
            if (modeFlyout.IsOpen()) modeFlyout.Hide();
            modePending=L""; modeGeneration=-1; modeApplied=0; modeReveal=0; revealModePending=false;
            pending = L""; enterPending = false; submitIntent = L""; queryComplete = false; acknowledgedEditId = 0; session = nextSession; sequence = -1; visibility = L""; editId = 0; selectionId = 0;
            observedText = field.Text(); observedComposing = composing;
        }
        root.Visibility(Visibility::Visible);
        Edit(); // Observe text synchronously before an older snapshot can replace it.
        if (!pending.empty()) { auto action = pending; pending = L""; return action; }
        if (!modePending.empty()) { auto action=modePending; modePending=L""; return action; }
        syncing = true;
        try {
            field.PlaceholderText(snapshot.GetNamedString(L"placeholder", L"Search or enter address"));
            const auto previousRevision = revision;
            revision = snapshot.GetNamedNumber(L"revision");
            acknowledgedEditId = snapshot.GetNamedNumber(L"editId", 0);
            queryComplete = snapshot.GetNamedBoolean(L"complete", false);
            auto text = snapshot.GetNamedString(L"text", L"");
            if (text.size() > 32768) winrt::throw_hresult(E_INVALIDARG);
            if (field.Text() != text) { observedText = text; observedComposing = composing; field.Text(text); field.Select(int(text.size()), 0); }
            auto nextSequence = snapshot.GetNamedNumber(L"sequence", 0);
            if (sequence != nextSequence || previousRevision != revision) {
                sequence = nextSequence;
                auto rows = snapshot.GetNamedArray(L"rows");
                if (rows.Size() > 100) winrt::throw_hresult(E_INVALIDARG);
                bool sameIds = rows.Size() == ids.size();
                for (uint32_t i = 0; sameIds && i < rows.Size(); ++i)
                    sameIds = rows.GetAt(i).GetObject().GetNamedString(L"id") == ids[i];
                if (!sameIds) { ids.clear(); visuals.clear(); list.Items().Clear(); }
                titles.clear(); actionLabels.clear(); visibility = L"";
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
                        visual.title.Text(title); visual.title.TextTrimming(TextTrimming::CharacterEllipsis);
                        visual.title.VerticalAlignment(VerticalAlignment::Center);
                        visual.section.TextWrapping(TextWrapping::Wrap);
                        Automation::AutomationProperties::SetHeadingLevel(visual.section, Automation::Peers::AutomationHeadingLevel::Level2);
                        StackPanel content; content.Spacing(4);
                        Grid line; line.ColumnSpacing(8);
                        ColumnDefinition iconColumn; iconColumn.Width(GridLengthHelper::Auto());
                        ColumnDefinition titleColumn; titleColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                        line.ColumnDefinitions().Append(iconColumn); line.ColumnDefinitions().Append(titleColumn);
                        Grid::SetColumn(visual.title, 1);
                        Grid icon;
                        icon.Children().Append(visual.symbol); icon.Children().Append(visual.image);
                        visual.image.Stretch(Media::Stretch::Uniform);
                        Automation::AutomationProperties::SetAccessibilityView(visual.image, Automation::Peers::AccessibilityView::Raw);
                        Automation::AutomationProperties::SetAccessibilityView(visual.symbol, Automation::Peers::AccessibilityView::Raw);
                        line.Children().Append(icon); line.Children().Append(visual.title);
                        content.Children().Append(visual.section); content.Children().Append(line);
                        container.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                        container.Content(content); container.Tag(winrt::box_value(id));
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
                    visual.image.Width(size); visual.image.Height(size);
                    visual.symbol.Width(size); visual.symbol.Height(size);
                    const auto symbol = row.GetNamedString(L"symbol", L"page");
                    visual.symbol.Symbol(symbol == L"search" ? Symbol::Find : symbol == L"history" ? Symbol::Clock : Symbol::Document);
                    const bool hasImage = row.HasKey(L"image") && row.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null;
                    if (hasImage) {
                        auto data = row.GetNamedObject(L"image");
                        auto pixels = data.GetNamedString(L"pixels");
                        const int width = int(data.GetNamedNumber(L"width")), height = int(data.GetNamedNumber(L"height"));
                        if (visual.pixels != pixels || visual.pixelWidth != width || visual.pixelHeight != height) {
                            visual.image.Source(NativeJson::DecodeImage(data)); visual.pixels = pixels;
                            visual.pixelWidth = width; visual.pixelHeight = height;
                        }
                    } else { visual.image.Source(nullptr); visual.pixels = L""; }
                    visual.image.Visibility(hasImage ? Visibility::Visible : Visibility::Collapsed);
                    visual.symbol.Visibility(hasImage ? Visibility::Collapsed : Visibility::Visible);
                }
            }
            auto selected = snapshot.GetNamedString(L"selectedId", L"");
            auto selectedIt = std::find(ids.begin(), ids.end(), selected);
            list.SelectedIndex(selectedIt == ids.end() ? -1 : int(selectedIt - ids.begin()));
            RefreshLabels();
            SyncModes(snapshot);
            SyncGlobal(snapshot);
        } catch (...) { syncing = false; throw; }
        syncing = false;
        if (revealModePending && modeButton.IsEnabled() && modeButton.Visibility()==Visibility::Visible) {
            revealModePending=false; modeFlyout.ShowAt(modeButton);
        }
        if (enterPending && (queryComplete || !ids.empty()) && acknowledgedEditId == editId) {
            enterPending = false;
            Action(L"activate");
            auto action = pending; pending = L""; return action;
        }
        winrt::Windows::Data::Json::JsonArray visible;
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto row = list.ContainerFromIndex(i).try_as<FrameworkElement>();
            if (!row || row.ActualHeight() <= 0) continue;
            auto origin = row.TransformToVisual(list).TransformPoint({0,0});
            if (origin.Y < list.ActualHeight() && origin.Y + row.ActualHeight() > 0)
                visible.Append(JValue::CreateStringValue(ids[i]));
        }
        std::set<winrt::hstring> visibleActions;
        for (auto const& visual:globalVisuals) {
            if (!root.XamlRoot() || visual.button.ActualHeight()<=0) continue;
            auto origin=visual.button.TransformToVisual(nullptr).TransformPoint({0,0}); auto size=root.XamlRoot().Size();
            if (origin.Y<size.Height && origin.Y+visual.button.ActualHeight()>0 && origin.X<size.Width &&
                origin.X+visual.button.ActualWidth()>0 && visibleActions.insert(visual.resultId).second)
                visible.Append(JValue::CreateStringValue(visual.resultId));
        }
        auto value = Message(L"visibility");
        value.SetNamedValue(L"sequence", JValue::CreateNumberValue(sequence));
        value.SetNamedValue(L"ids", visible);
        auto report = value.Stringify();
        if (report == visibility) return L"";
        visibility = report; return report;
    }
};
