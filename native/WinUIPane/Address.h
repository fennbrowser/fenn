#pragma once
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "JsonValidation.h"
#include "ShellCore.h"
#include "SearchMode.h"
#include "GlobalActions.h"
#include "SuggestionList.h"
#include "NavigationToolbar.h"
#include "AddressContextMenu.h"

struct NativeAddress {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using JValue = winrt::Windows::Data::Json::JsonValue;
    using JsonArray = winrt::Windows::Data::Json::JsonArray;
    // The larger favicons address rows can carry, vs. the icon-sized images
    // NativeJson::kIconImageLimits bounds for the mode switcher and global
    // actions below.
    static constexpr NativeJson::ImageLimits kRowImageLimits{104, 65536};
    Microsoft::UI::Xaml::Controls::StackPanel root;
    Microsoft::UI::Xaml::Controls::Grid toolbarRoot{nullptr}, toolbarSurface{nullptr};
    std::unique_ptr<NativeNavigationToolbar> navigation;
    std::function<void(winrt::hstring const&)> navigate;
    std::function<void()> releaseMenuFocus;
    Microsoft::UI::Xaml::Controls::TextBox field;
    NativeSearchMode mode;
    NativeAddressContextMenu context;
    double clipboardSerial{};
    PinnedExtensionButtons omnibarLeft, omnibarRight;
    SuggestionList suggestions;
    GlobalActionsPanel actions;
    Microsoft::UI::Xaml::Controls::Primitives::Popup popup{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel popupContent{nullptr};
    Microsoft::UI::Xaml::Controls::Button fallback;
    NativeShellCore& shell;
    // Cached WinUI/JSON observations; lifecycle decisions and IDs live in Rust.
    winrt::hstring session, pending, visibility, observedText;
    NativeShellCore::Intent StagedKind() const {
        return NativeShellCore::Intent(shell.AddressState().staged_kind);
    }
    // The visibility last reported, as a set of ids rather than a serialized
    // message, plus the sequence it was computed against.
    std::set<winrt::hstring> reportedVisible;
    double reportedSequence{-1};
    bool visibilityQueued{};
    bool focusObservationQueued{};
    // Hands an intent to the owning pane's request queue. Its notification
    // only posts host work, so the admitted ID is registered with Rust before
    // draining. Rust clears that identity on the matching outcome.
    std::function<uint64_t(winrt::hstring const&)> dispatch;
    // Reports which rows are on screen. A separate channel from `dispatch`
    // because a report is not an intent: it must not supersede one, and
    // answering it must not be read as answering one.
    std::function<void(winrt::hstring const&)> reportVisible;
    // Re-evaluates focus and stages the resulting transition. Supplied by the
    // owning pane, which alone holds the island and the HWND that the answer
    // depends on. Calling it is always safe: ObserveFocus acts only on a
    // change, so extra calls cost a focus query and nothing else.
    std::function<void()> observeFocus;

    // Focus moved somewhere inside this surface, or out of it. XAML routes
    // GotFocus/LostFocus up from the element that changed, so subscribing on
    // the root sees every transition within the address controls. The answer
    // is taken on the next dispatcher turn because at the moment these fire
    // the new focus has not settled -- during a move between two of our own
    // controls, LostFocus arrives before the new element has focus, and asking
    // then would report a spurious departure.
    void ScheduleFocusObservation() {
        if (focusObservationQueued) return;
        auto queue = Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!queue) return;
        focusObservationQueued = true;
        queue.TryEnqueue([this, alive = std::weak_ptr<int>(lifetime)] {
            if (alive.expired()) return;
            focusObservationQueued = false;
            if (observeFocus) observeFocus();
        });
    }
    double revision{}, sequence{-1}, selectionId{};
    bool syncing{}, composing{}, observedComposing{}, focusWithin{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~NativeAddress() { lifetime.reset(); if (popup) popup.IsOpen(false); }

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
        if (Decide(NativeShellCore::GlobalAction, StagedKind()) == NativeShellCore::DropIntent) return;
        auto value = Message(L"global-action");
        value.SetNamedValue(L"id", JValue::CreateStringValue(resultId));
        value.SetNamedValue(L"actionId", JValue::CreateStringValue(id));
        JObject intent;
        intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
        intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
        intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU) & 0x8000) != 0));
        value.SetNamedValue(L"intent", intent);
        Stage(value, NativeShellCore::GlobalAction);
        shell.CancelAddressSubmit();
    }
    // Everything the policy may consider, read from WinUI, Win32 and the last
    // applied publication. Assembled here so the rules cannot quietly start
    // depending on something else.
    ShellIntentConditions Observed() const {
        const auto state = shell.AddressState();
        return ShellIntentConditions{
            syncing ? 1u : 0u,
            composing ? 1u : 0u,
            session.empty() ? 0u : 1u,
            state.mode_pending,
            state.edit_acknowledged,
            state.query_settled,
        };
    }
    NativeShellCore::Decision Decide(NativeShellCore::Intent incoming,
                                     NativeShellCore::Intent staged) const {
        // The shared Rust policy combines control observations with core state.
        return shell.Decide(incoming, staged, Observed());
    }
    static NativeShellCore::Intent IntentOf(wchar_t const* kind) {
        if (wcscmp(kind, L"select") == 0) return NativeShellCore::Select;
        if (wcscmp(kind, L"activate") == 0) return NativeShellCore::Activate;
        if (wcscmp(kind, L"focus") == 0) return NativeShellCore::Focus;
        if (wcscmp(kind, L"blur") == 0) return NativeShellCore::Blur;
        if (wcscmp(kind, L"dismiss") == 0) return NativeShellCore::Dismiss;
        if (wcsncmp(kind, L"context-", 8) == 0) return NativeShellCore::ContextMenu;
        if (wcscmp(kind, L"clipboard") == 0) return NativeShellCore::Clipboard;
        if (wcscmp(kind, L"fallback") == 0) return NativeShellCore::Fallback;
        // Every kind Action() is called with must map to one: an unmapped kind
        // would be dropped silently, which is how the fallback button briefly
        // stopped working.
        return NativeShellCore::NoIntent;
    }
    bool Stage(JObject const& value, NativeShellCore::Intent kind) {
        auto action = value.Stringify();
        // Refusal has no completion. Keep the last admitted slot and its ID
        // intact; unadmitted edits are tracked separately below.
        const auto request = dispatch ? dispatch(action) : 0;
        if (!request || !shell.StageAddress(request, kind)) return false;
        pending = action;
        return true;
    }
    void ResetSessionState() {
        shell.ResetAddress();
        pending = L"";
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
        if (Decide(NativeShellCore::Edit, StagedKind()) == NativeShellCore::DropIntent) return;
        if (!shell.AddressEdit()) return;
        observedText = field.Text();
        observedComposing = composing;
        RetryEdit();
    }
    // Capacity becomes available at request completion. Retry only an edit
    // that was never admitted, preserving its ID and any early Enter intent.
    void RetryEdit() {
        const auto state = shell.AddressState();
        if (!state.needs_dispatch || session.empty()) return;
        auto value = Message(L"edit");
        value.SetNamedValue(L"editId", JValue::CreateNumberValue(double(state.edit_id)));
        value.SetNamedValue(L"text", JValue::CreateStringValue(observedText));
        value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
        value.SetNamedValue(L"selectionEnd",
                            JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
        value.SetNamedValue(L"composing", JValue::CreateBooleanValue(composing));
        Stage(value, NativeShellCore::Edit);
    }
    void Action(wchar_t const* kind, int traversal = 0, wchar_t const* clipboard = nullptr) {
        const auto incoming = IntentOf(kind);
        if (incoming == NativeShellCore::NoIntent) return;
        // Is this intent meaningful at all right now -- a question about the
        // surface, not about what is staged. Asked before Edit() below, which
        // is where the original guard sat.
        if (Decide(incoming, NativeShellCore::NoIntent) == NativeShellCore::DropIntent) return;
        if (incoming == NativeShellCore::Activate || incoming == NativeShellCore::Fallback || incoming == NativeShellCore::Clipboard || incoming == NativeShellCore::ContextMenu || incoming == NativeShellCore::Blur) Edit();
        const uint32_t modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                                   ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                                   ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                                   ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
        const auto action = shell.NegotiateAddress(incoming, Observed(), modifiers);
        if (action.decision == NativeShellCore::Defer || action.decision == NativeShellCore::DropIntent) return;
        auto value = Message(kind);
        if (incoming == NativeShellCore::Fallback || incoming == NativeShellCore::Clipboard || incoming == NativeShellCore::ContextMenu || incoming == NativeShellCore::Blur) {
            value.SetNamedValue(L"editId", JValue::CreateNumberValue(double(shell.AddressState().edit_id)));
            value.SetNamedValue(L"text", JValue::CreateStringValue(field.Text()));
            value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
            value.SetNamedValue(L"selectionEnd", JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
            value.SetNamedValue(L"composing", JValue::CreateBooleanValue(composing));
            if (traversal) value.SetNamedValue(L"traverse", JValue::CreateNumberValue(traversal));
            if (incoming == NativeShellCore::ContextMenu) {
                value.SetNamedValue(L"generation", JValue::CreateNumberValue(double(context.generation)));
                if (clipboard) value.SetNamedValue(L"id", JValue::CreateStringValue(clipboard));
            } else if (clipboard) value.SetNamedValue(L"clipboard", JValue::CreateStringValue(clipboard));
        }
        if (incoming == NativeShellCore::Activate) {
            JObject intent;
            intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((action.modifiers & 1) != 0));
            intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((action.modifiers & 2) != 0));
            intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((action.modifiers & 4) != 0));
            intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue((action.modifiers & 8) != 0));
            value.SetNamedValue(L"intent", intent);
        }
        int index = suggestions.list.SelectedIndex();
        if (incoming != NativeShellCore::ContextMenu && index >= 0 && size_t(index) < suggestions.ids.size())
            value.SetNamedValue(L"id", JValue::CreateStringValue(suggestions.ids[index]));
        Stage(value, incoming);
    }
    void ObserveFocus(bool islandHasFocus) {
        if ((mode.flyout.IsOpen() || context.IsOpen()) && islandHasFocus) return;
        bool inside = false;
        if (root.XamlRoot() && islandHasFocus) {
            auto focused =
                Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<DependencyObject>();
            while (focused) {
                // Site/page buttons live inside the editor's visual border,
                // but focusing them must not start or restore an address query.
                if (focused == omnibarLeft.panel || focused == omnibarRight.panel) break;
                if (focused == root || (popupContent && focused == popupContent)) {
                    inside = true;
                    break;
                }
                focused = Media::VisualTreeHelper::GetParent(focused);
            }
        }
        // XAML retains logical focus when Win32 focus leaves the island.
        if (inside != focusWithin) {
            focusWithin = inside;
            if (!inside) context.requested = false;
            Action(inside ? L"focus" : L"blur");
            if (GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0))
                fwprintf(stderr, L"Native address focus inside=%d island=%d pending=%s\n", inside, islandHasFocus,
                         StagedKind() == NativeShellCore::NoIntent ? L"none" : JObject::Parse(pending).GetNamedString(L"kind").c_str());
        }
    }
    explicit NativeAddress(NativeShellCore& core, bool toolbar = false) : mode(core), shell(core) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        root.Visibility(Visibility::Collapsed);
        root.Spacing(4);
        root.Margin({0, 0, 0, 12});
        field.PlaceholderText(L"Search or enter address");
        field.MaxLength(int32_t(NativeJson::kMaxFieldText));
        field.Loaded([](auto const& sender, auto const&) {
            auto editor = sender.template as<TextBox>();
            editor.ApplyTemplate();
            if (!Media::VisualTreeHelper::GetChildrenCount(editor)) return;
            auto templateRoot = Media::VisualTreeHelper::GetChild(editor, 0).try_as<FrameworkElement>();
            auto clear = templateRoot ? templateRoot.FindName(L"DeleteButton").try_as<Button>() : nullptr;
            if (!clear) return;
            // Keep the stock editor template and focus states. Its ButtonVisible
            // animation owns Visibility, so remove the clear affordance's size
            // and input instead of fighting that animation on every text edit.
            clear.MinWidth(0);
            clear.Width(0);
            clear.MaxWidth(0);
            clear.Opacity(0);
            clear.IsHitTestVisible(false);
            clear.IsEnabled(false);
        });
        Automation::AutomationProperties::SetName(field, L"Search or enter address");
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
            if (shell.AddressState().needs_dispatch) return;
            const auto decision = Decide(NativeShellCore::TextSelection, StagedKind());
            if (decision == NativeShellCore::Amend) {
                auto value = JObject::Parse(pending);
                value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
                value.SetNamedValue(L"selectionEnd",
                                    JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
                Stage(value, StagedKind());
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
                             [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                                 if (!alive.expired() && !e.GetCurrentPoint(field).Properties().IsRightButtonPressed()) Action(L"focus");
                             })),
                         true);
        field.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
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
        // Focus transitions used to be noticed only because syncAddress ran on
        // a timer and sampled them. These are what let that timer go.
        root.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
        });
        root.LostFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
        });
        // What is on screen changes when the list scrolls, when it is resized,
        // and when rows are realized. EffectiveViewportChanged is raised for
        // exactly those, which is what lets the visibility report stop being
        // sampled on a timer: it fires on the scroll that changed the answer,
        // immediately, and not otherwise.
        suggestions.list.EffectiveViewportChanged(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleVisibilityReport();
            });
        suggestions.list.SizeChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleVisibilityReport();
        });
        root.SizeChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            // The global-action buttons live outside the list and are clipped
            // by the pane itself, so they answer to the pane's own size.
            if (!alive.expired()) ScheduleVisibilityReport();
        });
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
        fallback.Content(winrt::box_value(L"Use Firefox address bar"));
        Automation::AutomationProperties::SetAutomationId(fallback, L"FirefoxWinUIAddressFallback");
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
        if (toolbar) {
            // Flatten the unfocused/hover border; leave the stock focused accent
            // and thickness intact. High contrast keeps its system borders.
            field.Resources(Markup::XamlReader::Load(LR"(
                <ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
                    <ResourceDictionary.ThemeDictionaries>
                        <ResourceDictionary x:Key="Default">
                            <SolidColorBrush x:Key="TextControlBorderBrush" Color="{ThemeResource ControlStrokeColorDefault}"/>
                            <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="{ThemeResource ControlStrokeColorDefault}"/>
                        </ResourceDictionary>
                        <ResourceDictionary x:Key="Light">
                            <SolidColorBrush x:Key="TextControlBorderBrush" Color="{ThemeResource ControlStrokeColorDefault}"/>
                            <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="{ThemeResource ControlStrokeColorDefault}"/>
                        </ResourceDictionary>
                        <ResourceDictionary x:Key="HighContrast">
                            <SolidColorBrush x:Key="TextControlBorderBrush" Color="{ThemeResource SystemColorButtonTextColor}"/>
                            <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="{ThemeResource SystemColorHighlightColor}"/>
                        </ResourceDictionary>
                    </ResourceDictionary.ThemeDictionaries>
                </ResourceDictionary>)").as<ResourceDictionary>());
            toolbarRoot = Grid();
            ColumnDefinition navigationColumn, addressColumn, menuColumn;
            menuColumn.Width(GridLengthHelper::Auto());
            navigationColumn.Width(GridLengthHelper::Auto());
            addressColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            toolbarRoot.ColumnDefinitions().Append(navigationColumn);
            toolbarRoot.ColumnDefinitions().Append(addressColumn);
            toolbarRoot.ColumnDefinitions().Append(menuColumn);
            navigation = std::make_unique<NativeNavigationToolbar>();
            navigation->activate = [this](wchar_t const* command, uint32_t modifiers, Controls::Button const& button) {
                if (syncing || session.empty() || !navigate) return;
                auto action = Message(L"navigation");
                action.SetNamedValue(L"command", JValue::CreateStringValue(command));
                action.SetNamedValue(L"modifiers", JValue::CreateNumberValue(modifiers));
                if (wcscmp(command, L"back-history") == 0 || wcscmp(command, L"forward-history") == 0 || wcscmp(command, L"profile") == 0 || wcscmp(command, L"extensions") == 0 || wcscmp(command, L"appmenu") == 0 || wcsncmp(command, L"extension", 9) == 0 || wcsncmp(command, L"omni", 4) == 0) {
                    auto local = button.TransformToVisual(nullptr).TransformBounds(
                        {0, 0, float(button.ActualWidth()), float(button.ActualHeight())});
                    auto screen = button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
                    action.SetNamedValue(L"x", JValue::CreateNumberValue(screen.X));
                    action.SetNamedValue(L"y", JValue::CreateNumberValue(screen.Y));
                    action.SetNamedValue(L"width", JValue::CreateNumberValue(screen.Width));
                    action.SetNamedValue(L"height", JValue::CreateNumberValue(screen.Height));
                    if (releaseMenuFocus) releaseMenuFocus();
                }
                navigate(action.Stringify());
            };
            omnibarLeft.activate = navigation->activate;
            omnibarRight.activate = navigation->activate;
            for (auto* buttons : {&omnibarLeft, &omnibarRight}) {
                buttons->commandPrefix = L"omni:";
                buttons->contextPrefix = L"omni-menu:";
                buttons->automationPrefix = L"FirefoxWinUIOmnibar-";
                buttons->buttonSize = 28;
                buttons->panel.Spacing(0);
                buttons->panel.VerticalAlignment(VerticalAlignment::Center);
            }
            omnibarLeft.panel.Margin({4, 0, 0, 0});
            omnibarRight.panel.Margin({0, 0, 4, 0});
            // The stock TextBox owns the entire surface and focus accent. Its
            // padding reserves the overlaid native controls within that border.
            header.Children().Clear();
            header.ColumnDefinitions().Clear();
            for (int i = 0; i < 5; ++i) {
                ColumnDefinition column;
                column.Width(i == 2 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
                header.ColumnDefinitions().Append(column);
            }
            Grid::SetColumn(field, 0); Grid::SetColumnSpan(field, 5);
            Grid::SetColumn(omnibarLeft.panel, 0);
            Grid::SetColumn(mode.button, 1);
            Grid::SetColumn(mode.exit, 3);
            Grid::SetColumn(omnibarRight.panel, 4);
            // Keep logical tab order left-to-right while painting the editor
            // behind the embedded controls.
            Canvas::SetZIndex(field, -1);
            header.Children().Append(omnibarLeft.panel);
            header.Children().Append(mode.button);
            header.Children().Append(field);
            header.Children().Append(mode.exit);
            header.Children().Append(omnibarRight.panel);
            mode.button.MinWidth(0); mode.button.MinHeight(0);
            mode.button.Height(24); mode.button.Margin({4, 0, 4, 0});
            // The stock 12-DIP arrow has internal side bearings; balance visible edge spacing.
            mode.button.Padding({8, 0, 6, 0});
            mode.button.VerticalAlignment(VerticalAlignment::Center);
            mode.button.BorderThickness({0, 0, 0, 0});
            mode.button.CornerRadius({12, 12, 12, 12});
            auto modeContent = mode.button.Content().as<StackPanel>();
            // Compact the stock template's arrow gap without shrinking the
            // content presenter's layout slot (negative margins overflow it).
            mode.button.Loaded([](auto const& sender, auto const&) {
                auto button = sender.template as<DropDownButton>();
                button.ApplyTemplate();
                if (!Media::VisualTreeHelper::GetChildrenCount(button)) return;
                auto templateRoot = Media::VisualTreeHelper::GetChild(button, 0).template try_as<FrameworkElement>();
                auto chevron = templateRoot ? templateRoot.FindName(L"ChevronIcon").template try_as<FrameworkElement>() : nullptr;
                if (chevron) chevron.Margin({6, 0, 0, 0});
            });
            modeContent.Children().RemoveAt(1);
            Viewbox modeIcon;
            modeIcon.Width(16); modeIcon.Height(16);
            modeIcon.Child(mode.symbol);
            modeIcon.Visibility(mode.symbol.Visibility());
            mode.symbolHost = modeIcon;
            modeContent.Children().InsertAt(1, modeIcon);
            auto reserveControls = [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired()) return;
                auto width = [](FrameworkElement const& element) {
                    return element.Visibility() == Visibility::Visible ? element.ActualWidth() + element.Margin().Left + element.Margin().Right : 0.0;
                };
                const float textGap = mode.button.Visibility() == Visibility::Visible ? 2.0f : 8.0f;
                field.Padding({float(textGap + width(omnibarLeft.panel) + width(mode.button)), 4,
                    float(8 + width(mode.exit) + width(omnibarRight.panel)), 4});
            };
            for (auto element : {omnibarLeft.panel.as<FrameworkElement>(), mode.button.as<FrameworkElement>(),
                                 mode.exit.as<FrameworkElement>(), omnibarRight.panel.as<FrameworkElement>()}) {
                element.SizeChanged(reserveControls);
                element.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), reserveControls);
            }
            toolbarSurface = Markup::XamlReader::Load(
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource LayerOnMicaBaseAltFillColorDefaultBrush}"/>)").as<Grid>();
            toolbarSurface.Visibility(Visibility::Collapsed);
            Grid::SetColumnSpan(toolbarSurface, 3);
            Grid::SetColumn(root, 1);
            toolbarRoot.Children().Append(toolbarSurface);
            toolbarRoot.Children().Append(navigation->panel);
            toolbarRoot.Children().Append(root);
            Grid::SetColumn(navigation->trailing, 2);
            toolbarRoot.Children().Append(navigation->trailing);
            root.VerticalAlignment(VerticalAlignment::Center);
            Automation::AutomationProperties::SetAutomationId(toolbarRoot, L"FirefoxWinUINavigationToolbar");
            field.ContextFlyout(nullptr);
            field.SelectionFlyout(nullptr);
            context.action = [this, alive = std::weak_ptr<int>(lifetime)](auto kind, auto const& id) {
                if (!alive.expired()) Action(kind, 0, id.c_str());
            };
            field.ContextRequested([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                if (alive.expired()) return;
                e.Handled(true);
                context.requested = true;
                context.atPointer = e.TryGetPosition(field, context.position);
                Action(L"context-open");
            });
            field.CopyingToClipboard([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                if (alive.expired()) return;
                e.Handled(true);
                Action(L"clipboard", 0, L"cmd_copy");
            });
            field.CuttingToClipboard([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                if (alive.expired()) return;
                e.Handled(true);
                Action(L"clipboard", 0, L"cmd_cut");
            });
            field.Paste([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                if (alive.expired()) return;
                e.Handled(true);
                Action(L"clipboard", 0, L"cmd_paste");
            });
            root.Margin({0, 0, 0, 0});
            // The popup floats outside layout; spacing after the header would
            // shift the visible editor above the navigation buttons' center.
            root.Spacing(0);
            field.MinHeight(32);
            field.Padding({8, 4, 8, 4});
            popupContent = StackPanel();
            popupContent.Spacing(4);
            popupContent.Children().Append(suggestions.list);
            popupContent.Children().Append(actions.panel);
            popupContent.Children().Append(fallback);
            auto border = Markup::XamlReader::Load(
                LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SolidBackgroundFillColorBaseBrush}" BorderBrush="{ThemeResource SurfaceStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="8" Padding="4"/>)").as<Border>();
            border.Child(popupContent);
            popup = Primitives::Popup();
            popup.Child(border);
            popup.ShouldConstrainToRootBounds(false);
            popup.PlacementTarget(header);
            popup.DesiredPlacement(Primitives::PopupPlacementMode::BottomEdgeAlignedLeft);
            root.Children().Append(popup);
            popup.Opened([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleVisibilityReport();
            });
            popupContent.SizeChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleVisibilityReport();
            });
            popupContent.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleFocusObservation();
            });
            popupContent.LostFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleFocusObservation();
            });
            popupContent.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
                if (!alive.expired() && !composing && e.Key() == winrt::Windows::System::VirtualKey::Escape) {
                    field.Focus(FocusState::Keyboard);
                    Action(L"dismiss");
                    e.Handled(true);
                }
            });
        } else {
            root.Children().Append(suggestions.list);
            root.Children().Append(actions.panel);
            root.Children().Append(fallback);
        }
    }
    void PlacePopup() {
        if (!popup || !root.XamlRoot()) return;
        popup.XamlRoot(root.XamlRoot());
        popupContent.Width(std::max(240.0, root.ActualWidth() - 10));
        popup.HorizontalOffset(0);
        // WinUI places the popup relative to the retained editor header.
        popup.VerticalOffset(0);
    }
    winrt::hstring Sync(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (snapshot.GetNamedNumber(L"version") != 1) winrt::throw_hresult(E_INVALIDARG);
        if (!snapshot.GetNamedBoolean(L"enabled", false)) {
            if (navigation) navigation->Apply({}, false);
            context.Reset();
            omnibarLeft.Apply({}, false); omnibarRight.Apply({}, false);
            if (popup) popup.IsOpen(false);
            mode.Disable();
            actions.Clear();
            syncing = true;
            root.Visibility(Visibility::Collapsed);
            suggestions.Clear();
            ResetSessionState();
            session = L"";
            syncing = false;
            return L"";
        }
        auto nextSession = snapshot.GetNamedString(L"session");
        NativeJson::RequireRevision(snapshot, L"revision");
        auto checkedSequence = snapshot.GetNamedNumber(L"sequence", 0);
        const auto checkedEdit = snapshot.HasKey(L"editId") ? NativeJson::RequireRevision(snapshot, L"editId") : 0;
        const auto checkedComplete = snapshot.GetNamedBoolean(L"complete", false);
        const auto checkedOpen = snapshot.GetNamedBoolean(L"open", false);
        const auto checkedRtl = snapshot.GetNamedBoolean(L"rtl", false);
        const auto navigationToolbar = snapshot.GetNamedBoolean(L"navigationToolbar", false);
        const auto navigationState = NativeNavigationToolbar::Read(snapshot);
        const auto pinnedState = PinnedExtensionButtons::Read(snapshot);
        const auto omnibarLeftState = PinnedExtensionButtons::Read(snapshot, L"omnibarLeft");
        const auto omnibarRightState = PinnedExtensionButtons::Read(snapshot, L"omnibarRight");
        auto checkedText = snapshot.GetNamedString(L"text", L"");
        auto clipboard = snapshot.GetNamedObject(L"clipboard", JObject());
        const auto clipboardId = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"serial") : 0;
        const auto clipboardEdit = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"editId") : 0;
        const auto clipboardStart = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"start") : 0;
        const auto clipboardEnd = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"end") : 0;
        if (clipboardStart > clipboardEnd || clipboardEnd > checkedText.size()) winrt::throw_hresult(E_INVALIDARG);
        const auto clipboardBefore = clipboard.GetNamedString(L"before", L"");
        const auto clipboardReplacement = clipboard.GetNamedString(L"replacement", L"");
        const auto replaceStart = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"replaceStart") : 0;
        const auto replaceEnd = clipboard.Size() ? NativeJson::RequireRevision(clipboard, L"replaceEnd") : 0;
        if (clipboardId && (clipboardBefore.size() > NativeJson::kMaxFieldText ||
            replaceStart > replaceEnd || replaceEnd > clipboardBefore.size() ||
            clipboardReplacement.size() > NativeJson::kMaxFieldText ||
            std::wstring(clipboardBefore).substr(0, replaceStart) + std::wstring(clipboardReplacement) +
            std::wstring(clipboardBefore).substr(replaceEnd) != std::wstring(checkedText)))
            winrt::throw_hresult(E_INVALIDARG);
        if (snapshot.HasKey(L"contextMenu")) {
            auto menu = snapshot.GetNamedObject(L"contextMenu");
            NativeJson::RequireRevision(menu, L"generation"); NativeJson::RequireRevision(menu, L"editId");
            size_t count = 0; NativeAddressContextMenu::Validate(menu.GetNamedArray(L"items"), count);
        }
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
            context.Reset();
            mode.Reset();
            clipboardSerial = 0;
            ResetSessionState();
            session = nextSession;
            sequence = -1;
            visibility = L"";
            selectionId = 0;
            observedText = field.Text();
            observedComposing = composing;
        }
        const auto flow = checkedRtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight;
        root.FlowDirection(flow);
        if (toolbarRoot) toolbarRoot.FlowDirection(flow);
        if (popupContent) popupContent.FlowDirection(flow);
        // Mirror the interface without reversing URL/text character order.
        field.TextReadingOrder(TextReadingOrder::DetectFromContent);
        // Left is the logical leading edge; WinUI mirrors it with FlowDirection.
        field.TextAlignment(TextAlignment::Left);
        root.Visibility(Visibility::Visible);
        if (toolbarRoot) {
            navigation->Apply(navigationState, navigationToolbar && snapshot.HasKey(L"navigation"));
            navigation->pinned.Apply(pinnedState, navigationToolbar);
            omnibarLeft.Apply(omnibarLeftState, navigationToolbar);
            omnibarRight.Apply(omnibarRightState, navigationToolbar);
            toolbarSurface.Visibility(navigationToolbar ? Visibility::Visible : Visibility::Collapsed);
            root.Margin(navigationToolbar ? Thickness{8, 8, 8, 8} : Thickness{0, 0, 0, 0});
        }
        Edit();  // Observe text synchronously before an older snapshot can replace it.
        RetryEdit();
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
        if (shell.AddressState().needs_dispatch || StagedKind() != NativeShellCore::NoIntent || shell.AddressState().mode_pending) return L"";
        if (clipboardId && clipboardEdit != shell.AddressState().edit_id) return L"";
        syncing = true;
        try {
            auto placeholder = snapshot.GetNamedString(L"placeholder", L"Search or enter address");
            NativeJson::RequireLength(placeholder, NativeJson::kMaxShortLabel);
            field.PlaceholderText(placeholder);
            // Firefox's own field is named by its placeholder text.
            if (!placeholder.empty()) Automation::AutomationProperties::SetName(field, placeholder);
            if (snapshot.HasKey(L"labels")) {
                auto labels = snapshot.GetNamedObject(L"labels");
                auto suggestionsName = labels.GetNamedString(L"suggestions", L"Address suggestions");
                auto actionsName = labels.GetNamedString(L"actions", L"Search actions");
                NativeJson::RequireLength(suggestionsName, NativeJson::kMaxShortLabel);
                NativeJson::RequireLength(actionsName, NativeJson::kMaxShortLabel);
                if (!suggestionsName.empty()) Automation::AutomationProperties::SetName(suggestions.list, suggestionsName);
                if (!actionsName.empty()) Automation::AutomationProperties::SetName(actions.panel, actionsName);
                fallback.Content(winrt::box_value(labels.GetNamedString(L"addressFallback", L"Use Firefox address bar")));
            }
            const auto previousRevision = revision;
            revision = snapshot.GetNamedNumber(L"revision");
            auto text = snapshot.GetNamedString(L"text", L"");
            if (text.size() > NativeJson::kMaxFieldText) winrt::throw_hresult(E_INVALIDARG);
            if (field.Text() != text) {
                observedText = text;
                observedComposing = composing;
                if (clipboardId > clipboardSerial && field.Text() == clipboardBefore) {
                    // Replace the selected range through the stock editor so
                    // Cut/Paste participate in its undo history.
                    field.Select(int(replaceStart), int(replaceEnd - replaceStart));
                    field.SelectedText(clipboardReplacement);
                } else field.Text(text);
                field.Select(int(text.size()), 0);
            }
            if (clipboardId > clipboardSerial) {
                field.Select(int(clipboardStart), int(clipboardEnd - clipboardStart));
                clipboardSerial = double(clipboardId);
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
            shell.PublishAddress(uint64_t(checkedEdit), checkedComplete || !suggestions.ids.empty());
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
        if (popup) {
            PlacePopup();
            popup.IsOpen(checkedOpen);
        }
        context.Sync(snapshot, shell.AddressState().edit_id, focusWithin, field);
        mode.ShowPendingReveal();
        // The deferred commit's precondition is satisfied by a publication, so
        // this is where it is re-offered. Action() queues it if the policy now
        // allows it; nothing is returned from here.
        if (shell.TakeDeferredAddress()) Action(L"activate");
        ReportVisibility();
        return L"";
    }

    // Which rows and global actions are actually on screen. This is the one
    // thing about this surface that no browser event can report: it is a fact
    // about native layout, so native is what observes it.
    //
    // Driven by the viewport events below rather than sampled on a timer, and
    // compared as a set of ids rather than by serialising the report and
    // comparing strings -- the old form allocated and stringified on every
    // tick purely to discover that nothing had changed.
    void ReportVisibility() {
        if (session.empty()) return;
        std::set<winrt::hstring> current;
        if (!popup || popup.IsOpen())
            for (auto const& id : suggestions.VisibleIds()) current.insert(id);
        for (auto const& visual : actions.visuals) {
            if (popup && !popup.IsOpen()) continue;
            if (!root.XamlRoot() || visual.button.ActualHeight() <= 0) continue;
            auto origin = visual.button.TransformToVisual(popupContent ? popupContent : root).TransformPoint({0, 0});
            auto size = popupContent ? Windows::Foundation::Size{float(popupContent.ActualWidth()), float(popupContent.ActualHeight())}
                                    : root.XamlRoot().Size();
            if (origin.Y < size.Height && origin.Y + visual.button.ActualHeight() > 0 && origin.X < size.Width &&
                origin.X + visual.button.ActualWidth() > 0)
                current.insert(visual.resultId);
        }
        if (current == reportedVisible && reportedSequence == sequence) return;
        reportedVisible = current;
        reportedSequence = sequence;
        JsonArray ids;
        for (auto const& id : current) ids.Append(JValue::CreateStringValue(id));
        auto value = Message(L"visibility");
        value.SetNamedValue(L"sequence", JValue::CreateNumberValue(sequence));
        value.SetNamedValue(L"ids", ids);
        if (reportVisible) reportVisible(value.Stringify());
    }

    // At most one visibility report is queued on the dispatcher at a time: a
    // scroll raises many viewport events and they all describe the same
    // answer, which is only worth computing once they have settled.
    void ScheduleVisibilityReport() {
        if (visibilityQueued || session.empty()) return;
        visibilityQueued = true;
        auto queue = Dispatching::DispatcherQueue::GetForCurrentThread();
        if (!queue) {
            visibilityQueued = false;
            return;
        }
        queue.TryEnqueue([this, alive = std::weak_ptr<int>(lifetime)] {
            if (alive.expired()) return;
            visibilityQueued = false;
            ReportVisibility();
        });
    }
};
