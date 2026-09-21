#pragma once
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "JsonValidation.h"
#include "ShellCore.h"
#include "SearchMode.h"
#include "GlobalActions.h"
#include "SuggestionList.h"

struct NativeAddress {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using JValue = winrt::Windows::Data::Json::JsonValue;
    // The larger favicons address rows can carry, vs. the icon-sized images
    // NativeJson::kIconImageLimits bounds for the mode switcher and global
    // actions below.
    static constexpr NativeJson::ImageLimits kRowImageLimits{104, 65536};
    Microsoft::UI::Xaml::Controls::StackPanel root;
    Microsoft::UI::Xaml::Controls::TextBox field;
    NativeSearchMode mode;
    SuggestionList suggestions;
    GlobalActionsPanel actions;
    winrt::hstring session, pending, visibility, observedText, submitIntent;
    // Which intent `pending` holds, as the shell core names it. Kept as a kind
    // rather than re-parsed out of `pending` on every decision, which is what
    // this code used to do to recover state it had just written.
    NativeShellCore::Intent stagedKind{NativeShellCore::NoIntent};
    // Supplied by the owning pane: the one definition of which intent survives
    // when two are in play. The rules live in native/ShellCore/src/intent.rs
    // where every pair is tested; nothing here re-implements them.
    std::function<NativeShellCore::Decision(NativeShellCore::Intent, NativeShellCore::Intent,
                                            ShellIntentConditions const&)>
        decide;
    // Hands one staged intent to the owning pane's request queue, which wakes
    // the host itself. The slot above remains the record of what is
    // outstanding -- the policy needs it -- and is cleared when the outcome
    // comes back rather than when a tick happens to collect it.
    std::function<void(winrt::hstring const&)> dispatch;
    double revision{}, sequence{-1}, editId{}, selectionId{}, acknowledgedEditId{};
    bool syncing{}, composing{}, enterPending{}, observedComposing{}, queryComplete{}, focusWithin{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~NativeAddress() { lifetime.reset(); }

    static void ValidateModeImage(JObject const& item, size_t& total) {
        NativeJson::ValidateImage(item, total, NativeJson::kIconImageLimits);
    }
    // Activating a global action shares the same pending-action bus as an
    // ordinary edit/select, so it must observe the same edit/session
    // coordination as Action() does -- kept here rather than in
    // GlobalActionsPanel, which has no access to that state.
    void TryGlobalAction(winrt::hstring const& id, winrt::hstring const& resultId) {
        Edit();
        // A global action names a row from the publication that produced it, so
        // it needs the browser level with our edits, and it yields to anything
        // staged except a caret move.
        if (Decide(NativeShellCore::GlobalAction, stagedKind) == NativeShellCore::DropIntent) return;
        auto value = Message(L"global-action");
        value.SetNamedValue(L"id", JValue::CreateStringValue(resultId));
        value.SetNamedValue(L"actionId", JValue::CreateStringValue(id));
        JObject intent;
        intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
        intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
        intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU) & 0x8000) != 0));
        value.SetNamedValue(L"intent", intent);
        Stage(value, NativeShellCore::GlobalAction);
        enterPending = false;
    }
    // Everything the policy may consider, read from WinUI, Win32 and the last
    // applied publication. Assembled here so the rules cannot quietly start
    // depending on something else.
    ShellIntentConditions Observed() const {
        return ShellIntentConditions{
            syncing ? 1u : 0u,
            composing ? 1u : 0u,
            session.empty() ? 0u : 1u,
            mode.pending.empty() ? 0u : 1u,
            acknowledgedEditId == editId ? 1u : 0u,
            (queryComplete || !suggestions.ids.empty()) ? 1u : 0u,
        };
    }
    NativeShellCore::Decision Decide(NativeShellCore::Intent incoming,
                                     NativeShellCore::Intent staged) const {
        // Without a decision function nothing is staged: dropping loses one
        // action, accepting could commit against the wrong suggestion.
        return decide ? decide(incoming, staged, Observed()) : NativeShellCore::DropIntent;
    }
    static NativeShellCore::Intent IntentOf(wchar_t const* kind) {
        if (wcscmp(kind, L"select") == 0) return NativeShellCore::Select;
        if (wcscmp(kind, L"activate") == 0) return NativeShellCore::Activate;
        if (wcscmp(kind, L"focus") == 0) return NativeShellCore::Focus;
        if (wcscmp(kind, L"blur") == 0) return NativeShellCore::Blur;
        if (wcscmp(kind, L"dismiss") == 0) return NativeShellCore::Dismiss;
        if (wcscmp(kind, L"fallback") == 0) return NativeShellCore::Fallback;
        // Every kind Action() is called with must map to one: an unmapped kind
        // would be dropped silently, which is how the fallback button briefly
        // stopped working.
        return NativeShellCore::NoIntent;
    }
    void Stage(JObject const& value, NativeShellCore::Intent kind) {
        pending = value.Stringify();
        stagedKind = kind;
        if (dispatch) dispatch(pending);
    }
    void ClearStaged() {
        pending = L"";
        stagedKind = NativeShellCore::NoIntent;
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
        if (field.Text() == observedText && composing == observedComposing) return;
        // A text change replaces whatever was staged: what the user can see is
        // the newest thing they have said. Composition start and end are edits
        // too, which is why this is the one intent not suppressed by an active
        // composition.
        if (Decide(NativeShellCore::Edit, stagedKind) == NativeShellCore::DropIntent) return;
        observedText = field.Text();
        observedComposing = composing;
        enterPending = false;
        submitIntent = L"";
        auto value = Message(L"edit");
        value.SetNamedValue(L"editId", JValue::CreateNumberValue(++editId));
        value.SetNamedValue(L"text", JValue::CreateStringValue(field.Text()));
        value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
        value.SetNamedValue(L"selectionEnd",
                            JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
        value.SetNamedValue(L"composing", JValue::CreateBooleanValue(composing));
        Stage(value, NativeShellCore::Edit);
    }
    void Action(wchar_t const* kind) {
        const auto incoming = IntentOf(kind);
        if (incoming == NativeShellCore::NoIntent) return;
        // Is this intent meaningful at all right now -- a question about the
        // surface, not about what is staged. Asked before Edit() below, which
        // is where the original guard sat.
        if (Decide(incoming, NativeShellCore::NoIntent) == NativeShellCore::DropIntent) return;
        if (incoming == NativeShellCore::Activate) {
            Edit();
            if (submitIntent.empty()) {
                JObject intent;
                intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
                intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU) & 0x8000) != 0));
                intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
                intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue((GetKeyState(VK_LWIN) & 0x8000) != 0));
                submitIntent = intent.Stringify();
            }
        }
        // Does it beat what is staged, and are its own preconditions met? The
        // deferral cases (committing before the browser has acknowledged the
        // edit, or behind an edit still in flight) come back as Defer.
        switch (Decide(incoming, stagedKind)) {
            case NativeShellCore::Defer:
                enterPending = true;
                return;
            case NativeShellCore::DropIntent:
                return;
            default:
                break;
        }
        if (incoming == NativeShellCore::Dismiss || incoming == NativeShellCore::Blur) {
            enterPending = false;
            submitIntent = L"";
        }
        auto value = Message(kind);
        if (wcscmp(kind, L"activate") == 0) {
            value.SetNamedValue(L"intent", JObject::Parse(submitIntent));
            submitIntent = L"";
        }
        int index = suggestions.list.SelectedIndex();
        if (index >= 0 && size_t(index) < suggestions.ids.size())
            value.SetNamedValue(L"id", JValue::CreateStringValue(suggestions.ids[index]));
        Stage(value, incoming);
    }
    void ObserveFocus(bool islandHasFocus) {
        if (mode.flyout.IsOpen()) return;
        bool inside = false;
        if (root.XamlRoot() && islandHasFocus) {
            auto focused =
                Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<DependencyObject>();
            while (focused) {
                if (focused == root) {
                    inside = true;
                    break;
                }
                focused = Media::VisualTreeHelper::GetParent(focused);
            }
        }
        // XAML retains logical focus when Win32 focus leaves the island.
        if (inside != focusWithin) {
            focusWithin = inside;
            Action(inside ? L"focus" : L"blur");
            if (GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0))
                fwprintf(stderr, L"Native address focus inside=%d island=%d pending=%s\n", inside, islandHasFocus,
                         pending.empty() ? L"none" : JObject::Parse(pending).GetNamedString(L"kind").c_str());
        }
    }
    NativeAddress() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        root.Visibility(Visibility::Collapsed);
        root.Spacing(4);
        root.Margin({0, 0, 0, 12});
        field.PlaceholderText(L"Search or enter address");
        field.MaxLength(int32_t(NativeJson::kMaxFieldText));
        Automation::AutomationProperties::SetName(field, L"Native address prototype");
        Automation::AutomationProperties::SetAutomationId(field, L"FirefoxWinUIAddress");
        field.TextChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            Edit();
        });
        field.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            // A caret move is the least important intent there is: it folds
            // into a staged edit, which already carries a range, and otherwise
            // never displaces anything.
            const auto decision = Decide(NativeShellCore::TextSelection, stagedKind);
            if (decision == NativeShellCore::Amend) {
                auto value = JObject::Parse(pending);
                value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
                value.SetNamedValue(L"selectionEnd",
                                    JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
                Stage(value, stagedKind);
                return;
            }
            if (decision != NativeShellCore::Stage && decision != NativeShellCore::Replace) return;
            auto value = Message(L"text-selection");
            value.SetNamedValue(L"selectionId", JValue::CreateNumberValue(++selectionId));
            value.SetNamedValue(L"start", JValue::CreateNumberValue(field.SelectionStart()));
            value.SetNamedValue(L"end", JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
            Stage(value, NativeShellCore::TextSelection);
        });
        field.TextCompositionStarted([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            composing = true;
            Edit();
        });
        field.TextCompositionEnded([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            composing = false;
            Edit();
        });
        // TextBox handles pointer presses internally; observe without handling
        // them again so native caret placement and selection remain standard.
        field.AddHandler(UIElement::PointerPressedEvent(),
                         winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                             [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                                 if (!alive.expired()) Action(L"focus");
                             })),
                         true);
        field.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) Action(L"focus");
        });
        field.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                 auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (alive.expired()) return;
            if (composing) return;
            using winrt::Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Enter) {
                Action(L"activate");
                e.Handled(true);
            }
            if (e.Key() == VirtualKey::Escape) {
                Action(L"dismiss");
                e.Handled(true);
            }
            if (e.Key() == VirtualKey::Down && !suggestions.ids.empty() && !(GetKeyState(VK_MENU) & 0x8000)) {
                auto& list = suggestions.list;
                int next = list.SelectedIndex() + 1;
                while (next < int(suggestions.ids.size()) && !list.Items().GetAt(next).as<ListViewItem>().IsEnabled())
                    ++next;
                if (next >= int(suggestions.ids.size())) {
                    e.Handled(true);
                    return;
                }
                syncing = true;
                list.Focus(FocusState::Keyboard);
                list.SelectedIndex(next);
                syncing = false;
                suggestions.RefreshLabels();
                Action(L"select");
                e.Handled(true);
            }
        });
        suggestions.list.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (!syncing) suggestions.RefreshLabels();
            Action(L"select");
        });
        suggestions.list.IsItemClickEnabled(true);
        suggestions.list.ItemClick(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, ItemClickEventArgs const& e) {
                if (alive.expired()) return;
                auto& list = suggestions.list;
                uint32_t index{};
                auto clicked = e.ClickedItem();
                bool found = list.Items().IndexOf(clicked, index);
                // Explicit ListViewItem entries report their Content as ClickedItem.
                // Match that exact object; never infer the clicked row from selection.
                if (!found) {
                    for (uint32_t i = 0; i < list.Items().Size(); ++i) {
                        auto container = list.Items().GetAt(i).try_as<ListViewItem>();
                        if (container && container.Content() == clicked) {
                            index = i;
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    syncing = true;
                    list.SelectedIndex(int(index));
                    syncing = false;
                    Action(L"activate");
                }
            });
        suggestions.list.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                            auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (alive.expired()) return;
            using winrt::Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Enter) {
                Action(L"activate");
                e.Handled(true);
            }
            if (e.Key() == VirtualKey::Escape) {
                Action(L"dismiss");
                e.Handled(true);
                field.Focus(FocusState::Keyboard);
            }
        });
        Button fallback;
        fallback.Content(winrt::box_value(L"Use Firefox address bar"));
        fallback.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            Action(L"fallback");
        });
        Grid header;
        ColumnDefinition buttonColumn;
        buttonColumn.Width(GridLengthHelper::Auto());
        ColumnDefinition fieldColumn;
        fieldColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition exitColumn;
        exitColumn.Width(GridLengthHelper::Auto());
        header.ColumnDefinitions().Append(buttonColumn);
        header.ColumnDefinitions().Append(fieldColumn);
        header.ColumnDefinitions().Append(exitColumn);
        Grid::SetColumn(field, 1);
        Grid::SetColumn(mode.exit, 2);
        mode.edit = [this] { Edit(); };
        mode.message = [this](wchar_t const* kind) { return Message(kind); };
        mode.clearIntent = [this] {
            enterPending = false;
            submitIntent = L"";
        };
        mode.focusField = [this] { field.Focus(FocusState::Programmatic); };
        mode.busy = [this] { return syncing || composing || session.empty(); };
        field.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                 auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!alive.expired() && (GetKeyState(VK_MENU) & 0x8000) &&
                (e.Key() == winrt::Windows::System::VirtualKey::Down ||
                 e.Key() == winrt::Windows::System::VirtualKey::Up)) {
                mode.Queue(L"mode-reveal");
                e.Handled(true);
            }
        });
        header.Children().Append(mode.button);
        header.Children().Append(field);
        header.Children().Append(mode.exit);
        actions.activate = [this](winrt::hstring const& id, winrt::hstring const& resultId) {
            TryGlobalAction(id, resultId);
        };
        actions.dismiss = [this] {
            Action(L"dismiss");
            field.Focus(FocusState::Keyboard);
        };
        actions.focusField = [this] { field.Focus(FocusState::Keyboard); };
        field.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                 auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (!alive.expired() && !composing && e.Key() == winrt::Windows::System::VirtualKey::Tab &&
                !(GetKeyState(VK_SHIFT) & 0x8000) && !actions.visuals.empty()) {
                actions.visuals.front().button.Focus(FocusState::Keyboard);
                e.Handled(true);
            }
        });
        root.Children().Append(header);
        root.Children().Append(suggestions.list);
        root.Children().Append(actions.panel);
        root.Children().Append(fallback);
    }
    winrt::hstring Sync(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (snapshot.GetNamedNumber(L"version") != 1) winrt::throw_hresult(E_INVALIDARG);
        if (!snapshot.GetNamedBoolean(L"enabled", false)) {
            mode.Disable();
            actions.Clear();
            syncing = true;
            root.Visibility(Visibility::Collapsed);
            suggestions.Clear();
            ClearStaged();
            session = L"";
            enterPending = false;
            submitIntent = L"";
            queryComplete = false;
            acknowledgedEditId = 0;
            syncing = false;
            return L"";
        }
        auto nextSession = snapshot.GetNamedString(L"session");
        NativeJson::RequireRevision(snapshot, L"revision");
        auto checkedSequence = snapshot.GetNamedNumber(L"sequence", 0);
        auto checkedText = snapshot.GetNamedString(L"text", L"");
        auto checkedRows = snapshot.GetNamedArray(L"rows");
        if (nextSession.empty() || nextSession.size() > 256 || checkedText.size() > NativeJson::kMaxFieldText ||
            checkedRows.Size() > NativeJson::kMaxListEntries)
            winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> checkedIds;
        size_t imagePixels = 0;
        for (auto value : checkedRows) {
            auto row = value.GetObject();
            auto id = row.GetNamedString(L"id");
            if (id.empty() || id.size() > NativeJson::kMaxOpaqueId || !checkedIds.insert(id).second ||
                row.GetNamedString(L"title").size() > 4096 ||
                row.GetNamedString(L"actionLabel", L"").size() > NativeJson::kMaxShortLabel)
                winrt::throw_hresult(E_INVALIDARG);
            const auto size = row.GetNamedNumber(L"imageSize", 16);
            if (size < 16 || size > 52 || row.GetNamedString(L"sectionLabel", L"").size() > NativeJson::kMaxShortLabel)
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::ValidateImage(row, imagePixels, kRowImageLimits);
        }
        auto globalActions = snapshot.GetNamedArray(L"globalActions", winrt::Windows::Data::Json::JsonArray());
        if (globalActions.Size() > 16) winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> actionIds;
        for (auto value : globalActions) {
            auto item = value.GetObject();
            auto id = item.GetNamedString(L"id");
            auto resultId = item.GetNamedString(L"resultId");
            if (id.empty() || id.size() > NativeJson::kMaxOpaqueId || !actionIds.insert(id).second ||
                resultId.empty() || resultId.size() > NativeJson::kMaxOpaqueId ||
                item.GetNamedString(L"title").size() > NativeJson::kMaxShortLabel ||
                item.GetNamedString(L"hint", L"").size() > NativeJson::kMaxShortLabel)
                winrt::throw_hresult(E_INVALIDARG);
            size_t pixels = 0;
            ValidateModeImage(item, pixels);
        }
        if (snapshot.HasKey(L"modes") &&
            snapshot.GetNamedValue(L"modes").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) {
            auto modes = snapshot.GetNamedObject(L"modes");
            auto entries = modes.GetNamedArray(L"entries");
            if (entries.Size() > NativeJson::kMaxListEntries ||
                modes.GetNamedString(L"label").size() > NativeJson::kMaxShortLabel ||
                modes.GetNamedString(L"buttonLabel", L"").size() > NativeJson::kMaxShortLabel ||
                modes.GetNamedString(L"exitLabel", L"").size() > NativeJson::kMaxShortLabel)
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireRevision(modes, L"generation");
            if (nextSession == session && modes.GetNamedNumber(L"generation") == mode.generation) {
                if (entries.Size() != mode.ids.size()) winrt::throw_hresult(E_INVALIDARG);
                for (uint32_t i = 0; i < entries.Size(); ++i)
                    if (entries.GetAt(i).GetObject().GetNamedString(L"id") != mode.ids[i])
                        winrt::throw_hresult(E_INVALIDARG);
            }
            size_t pixels = 0;
            ValidateModeImage(modes, pixels);
            std::set<winrt::hstring> unique;
            for (auto item : entries) {
                auto entry = item.GetObject();
                auto id = entry.GetNamedString(L"id");
                if (id.empty() || id.size() > NativeJson::kMaxOpaqueId || !unique.insert(id).second ||
                    entry.GetNamedString(L"label").size() > NativeJson::kMaxShortLabel)
                    winrt::throw_hresult(E_INVALIDARG);
                ValidateModeImage(entry, pixels);
            }
        }
        if (nextSession != session) {
            mode.Reset();
            ClearStaged();
            enterPending = false;
            submitIntent = L"";
            queryComplete = false;
            acknowledgedEditId = 0;
            session = nextSession;
            sequence = -1;
            visibility = L"";
            editId = 0;
            selectionId = 0;
            observedText = field.Text();
            observedComposing = composing;
        }
        root.Visibility(Visibility::Visible);
        Edit();  // Observe text synchronously before an older snapshot can replace it.
        // Intents are no longer collected here: Stage() and NativeSearchMode::Queue()
        // hand them straight to the request queue, which wakes the host itself.
        // Both slots stay as the record of what is outstanding, cleared when the
        // outcome arrives. The only thing this call still reports is the
        // visibility of rows, which is observed from layout and has no other
        // signal behind it.
        //
        // A publication that was produced before an outstanding intent predates
        // it by definition, so applying it would overwrite what the user has
        // just done -- the field would snap back to the text the browser knew
        // about when it published. The collection this replaced returned early
        // for the same reason; it is stated here rather than implied by where
        // the old code happened to return.
        if (stagedKind != NativeShellCore::NoIntent || !mode.pending.empty()) return L"";
        syncing = true;
        try {
            field.PlaceholderText(snapshot.GetNamedString(L"placeholder", L"Search or enter address"));
            const auto previousRevision = revision;
            revision = snapshot.GetNamedNumber(L"revision");
            acknowledgedEditId = snapshot.GetNamedNumber(L"editId", 0);
            queryComplete = snapshot.GetNamedBoolean(L"complete", false);
            auto text = snapshot.GetNamedString(L"text", L"");
            if (text.size() > NativeJson::kMaxFieldText) winrt::throw_hresult(E_INVALIDARG);
            if (field.Text() != text) {
                observedText = text;
                observedComposing = composing;
                field.Text(text);
                field.Select(int(text.size()), 0);
            }
            auto nextSequence = snapshot.GetNamedNumber(L"sequence", 0);
            if (sequence != nextSequence || previousRevision != revision) {
                sequence = nextSequence;
                suggestions.SyncRows(snapshot.GetNamedArray(L"rows"));
                visibility = L"";
            }
            suggestions.SyncSelection(snapshot.GetNamedString(L"selectedId", L""));
            suggestions.RefreshLabels();
            mode.Sync(snapshot);
            actions.Sync(snapshot);
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
        mode.ShowPendingReveal();
        // The deferred commit's precondition is satisfied by a publication, so
        // this is where it is re-offered. Action() queues it if the policy now
        // allows it; nothing is returned from here.
        if (enterPending && (queryComplete || !suggestions.ids.empty()) && acknowledgedEditId == editId) {
            enterPending = false;
            Action(L"activate");
        }
        winrt::Windows::Data::Json::JsonArray visible;
        suggestions.CollectVisibleIds(visible);
        std::set<winrt::hstring> visibleActions;
        for (auto const& visual : actions.visuals) {
            if (!root.XamlRoot() || visual.button.ActualHeight() <= 0) continue;
            auto origin = visual.button.TransformToVisual(nullptr).TransformPoint({0, 0});
            auto size = root.XamlRoot().Size();
            if (origin.Y < size.Height && origin.Y + visual.button.ActualHeight() > 0 && origin.X < size.Width &&
                origin.X + visual.button.ActualWidth() > 0 && visibleActions.insert(visual.resultId).second)
                visible.Append(JValue::CreateStringValue(visual.resultId));
        }
        auto value = Message(L"visibility");
        value.SetNamedValue(L"sequence", JValue::CreateNumberValue(sequence));
        value.SetNamedValue(L"ids", visible);
        auto report = value.Stringify();
        if (report == visibility) return L"";
        visibility = report;
        return report;
    }
};
