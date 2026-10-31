#pragma once
#include <msctf.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "JsonValidation.h"
#include "ShellCore.h"
#include "AddressInteraction.h"
#include "AddressValue.h"
#include "SearchMode.h"
#include "GlobalActions.h"
#include "SuggestionList.h"
#include "NavigationToolbar.h"
#include "AddressContextMenu.h"
#include "PageActionsMenu.h"
#include "BookmarkPanel.h"
#include "SitePermissionsPanel.h"
#include "NotificationPanel.h"
#include "ConnectionPanel.h"
#include "TranslationsPanel.h"
#include "SelectedTranslationsPanel.h"
#include "PageContextMenu.h"

// Win32 owns keyboard-layout translation. Probe without consuming dead-key state.
inline bool NativeAltGrTextKey(UINT key) {
    BYTE state[256]{};
    if (!GetKeyboardState(state) || !(state[VK_CONTROL] & 0x80) || !(state[VK_MENU] & 0x80)) return false;
    const auto layout = GetKeyboardLayout(0);
    wchar_t text[8]{};
    const auto count = ToUnicodeEx(key, MapVirtualKeyExW(key, MAPVK_VK_TO_VSC, layout),
                                   state, text, 8, 4, layout);
    return count < 0 || (count > 0 && text[0] >= L' ');
}

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
    // An explicitly unbound presenter owns local text without creating a
    // Firefox address session. Clear both callbacks only at acknowledged bind.
    std::function<void(winrt::hstring const&)> startupEdit;
    std::function<void(uint32_t)> startupSubmit;
    std::function<void()> releaseMenuFocus;
    std::function<void()> releaseContentFocus;
    std::function<void()> preparePointerFocus;
    Microsoft::UI::Xaml::Controls::TextBox field;
    NativeAddressValue formattedValue;
    NativeSearchMode mode;
    NativeAddressContextMenu context, resultMenu;
    winrt::hstring resultMenuId, resultMenuButtonId;
    double clipboardSerial{}, autofillSerial{}, textEpoch{};
    winrt::hstring nextInputType{L"insertReplacementText"}, observedInputType{L"insertReplacementText"};
    PinnedExtensionButtons omnibarLeft, omnibarRight;
    NativePageActionsMenu pageActions;
    NativeBookmarkPanel bookmarkPanel;
    NativeSitePermissionsPanel permissionsPanel;
    NativeNotificationPanel notificationPanel;
    NativeConnectionPanel connectionPanel;
    NativeTranslationsPanel translationsPanel;
    NativeSelectedTranslationsPanel selectedTranslationsPanel;
    NativePageContextMenu pageContextMenu;
    Microsoft::UI::Xaml::Controls::StackPanel trailingActions;
    SuggestionList suggestions;
    GlobalActionsPanel actions;
    Microsoft::UI::Xaml::Controls::Primitives::Popup popup{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel popupContent{nullptr};
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
    bool visibilityQueued{}, visibilityLayoutPending{};
    Microsoft::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker visibilityScroll;
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
    // Firefox asked for this focus, so the "focus" action it produces is an echo.
    // Firefox may have moved focus on by the time the echo drains; see FocusAddress.
    bool firefoxFocusRequest{};

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
    bool syncing{}, composing{}, observedComposing{};
    NativeAddressInteraction interaction;
    bool EditorOwned() const {
        return islandFocusOwned && field.XamlRoot() &&
            Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(field.XamlRoot()) == field;
    }
    // Read the current native STA's existing TSF context; never activate a second
    // manager or ask Gecko. Korean can retain a live syllable between TextBox
    // composition events, so the event flag alone is insufficient for adoption.
    bool HasActiveComposition() const {
        if (composing) return true;
        if (!EditorOwned()) return false;
        auto module=GetModuleHandleW(L"msctf.dll");
        using GetManager=HRESULT(WINAPI*)(ITfThreadMgr**);
        auto getManager=module?reinterpret_cast<GetManager>(GetProcAddress(module,"TF_GetThreadMgr")):nullptr;
        winrt::com_ptr<ITfThreadMgr> manager;
        winrt::com_ptr<ITfDocumentMgr> document;
        winrt::com_ptr<ITfContext> context;
        if(!getManager || FAILED(getManager(manager.put())) || !manager ||
            FAILED(manager->GetFocus(document.put())) || !document ||
            FAILED(document->GetTop(context.put())) || !context)return false;
        auto composition=context.try_as<ITfContextComposition>();
        winrt::com_ptr<IEnumITfCompositionView> entries;
        if(!composition || FAILED(composition->EnumCompositions(entries.put())) || !entries)return false;
        winrt::com_ptr<ITfCompositionView> entry;
        ULONG fetched{};
        return SUCCEEDED(entries->Next(1,entry.put(),&fetched)) && fetched==1;
    }
    void ApplyInteraction(ShellAddressInteractionState const& effect) {
        if (!interaction.Current(effect)) return;
        if (effect.effects & SHELL_ADDRESS_INTERACTION_EFFECT_CLEAR_SELECTION) {
            context.requested = false;
            resultMenu.requested = false;
            if (field.SelectionLength()) field.Select(0,0);
        }
        if (!interaction.Current(effect)) return;
        if ((effect.effects & SHELL_ADDRESS_INTERACTION_EFFECT_SELECT_ALL) && EditorOwned() &&
            !composing && field.SelectionLength() == 0) field.SelectAll();
        if (!interaction.Current(effect)) return;
        if ((effect.effects & SHELL_ADDRESS_INTERACTION_EFFECT_RELEASE_CONTENT_FOCUS) && releaseContentFocus) releaseContentFocus();
        if (!interaction.Current(effect)) return;
        if (effect.effects & SHELL_ADDRESS_INTERACTION_EFFECT_BLUR) Action(L"blur");
        if ((effect.effects & SHELL_ADDRESS_INTERACTION_EFFECT_FOCUS) && interaction.Current(effect)) Action(L"focus");
    }
    // In the navigation row the field's embedded buttons stay hidden until the
    // pointer is over the field, or keyboard focus or an open menu is within it.
    bool hideEmbeddedControls{}, islandFocusOwned{};
    Microsoft::UI::Xaml::Controls::Grid fieldHeader{nullptr};
    bool FocusInsideField() const {
        if (!islandFocusOwned || !fieldHeader || !fieldHeader.XamlRoot()) return false;
        auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(fieldHeader.XamlRoot()).try_as<winrt::Microsoft::UI::Xaml::DependencyObject>();
        while (focused) {
            if (focused == fieldHeader) return true;
            focused = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(focused);
        }
        return false;
    }
    bool PointerInsideField() const {
        if (!fieldHeader || !fieldHeader.XamlRoot()) return false;
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return false;
        auto point = fieldHeader.XamlRoot().ContentIsland().CoordinateConverter().ConvertScreenToLocal(
            winrt::Windows::Graphics::PointInt32{cursor.x,cursor.y});
        point = fieldHeader.TransformToVisual(nullptr).Inverse().TransformPoint(point);
        return point.X >= 0 && point.Y >= 0 && point.X < fieldHeader.ActualWidth() && point.Y < fieldHeader.ActualHeight();
    }
    void UpdateEmbeddedControls() {
        formattedValue.Refresh(field, EditorOwned() || composing);
        const bool reveal = !hideEmbeddedControls || PointerInsideField() || (islandFocusOwned &&
                            (FocusInsideField() || mode.flyout.IsOpen() || context.IsOpen() || resultMenu.IsOpen() || pageActions.open));
        omnibarLeft.SetRevealed(reveal);
        omnibarRight.SetRevealed(reveal);
        mode.button.Opacity(1.0);
        for (auto const& element : {winrt::Microsoft::UI::Xaml::UIElement(mode.exit),
                                    winrt::Microsoft::UI::Xaml::UIElement(pageActions.button)}) {
            element.Opacity(reveal ? 1.0 : 0.0);
        }
    }
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
        if (startupEdit) { if (!syncing) startupEdit(field.Text()); return; }
        if (field.Text() == observedText && composing == observedComposing) return;
        // A text change replaces whatever was staged: what the user can see is
        // the newest thing they have said. Composition start and end are edits
        // too, which is why this is the one intent not suppressed by an active
        // composition.
        if (Decide(NativeShellCore::Edit, StagedKind()) == NativeShellCore::DropIntent) return;
        if (!shell.AddressEdit()) return;
        observedText = field.Text();
        observedComposing = composing;
        observedInputType = nextInputType;
        nextInputType = L"insertReplacementText";
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
        value.SetNamedValue(L"inputType", JValue::CreateStringValue(observedInputType));
        value.SetNamedValue(L"textEpoch", JValue::CreateNumberValue(textEpoch));
        value.SetNamedValue(L"selectionStart", JValue::CreateNumberValue(field.SelectionStart()));
        value.SetNamedValue(L"selectionEnd",
                            JValue::CreateNumberValue(field.SelectionStart() + field.SelectionLength()));
        value.SetNamedValue(L"composing", JValue::CreateBooleanValue(composing));
        Stage(value, NativeShellCore::Edit);
    }
    void Action(wchar_t const* kind, int traversal = 0, wchar_t const* clipboard = nullptr) {
        if (startupEdit) {
            if (!wcscmp(kind,L"activate") && !composing && startupSubmit)
                startupSubmit(((GetKeyState(VK_SHIFT)&0x8000)?1u:0u) |
                              ((GetKeyState(VK_CONTROL)&0x8000)?2u:0u) |
                              ((GetKeyState(VK_MENU)&0x8000)?4u:0u));
            return;
        }
        const auto incoming = IntentOf(kind);
        if (incoming == NativeShellCore::NoIntent) return;
        const bool firefoxRequested = incoming == NativeShellCore::Focus && firefoxFocusRequest;
        if (incoming == NativeShellCore::Focus) firefoxFocusRequest = false;
        if (incoming == NativeShellCore::Dismiss || incoming == NativeShellCore::Activate ||
            incoming == NativeShellCore::Fallback || incoming == NativeShellCore::ContextMenu ||
            incoming == NativeShellCore::Clipboard) interaction.Interrupt(EditorOwned());
        const auto selected = suggestions.list.SelectedIndex();
        if (incoming == NativeShellCore::Select && !traversal &&
            selected >= 0 && size_t(selected) < suggestions.current.size() && !suggestions.current[selected]) return;
        // Is this intent meaningful at all right now -- a question about the
        // surface, not about what is staged. Asked before Edit() below, which
        // is where the original guard sat.
        if (Decide(incoming, NativeShellCore::NoIntent) == NativeShellCore::DropIntent) return;
        if (incoming == NativeShellCore::Select && suggestions.list.SelectedItem()) {
            // Keyboard selection can arrive before the stock list's deferred
            // bring-into-view. Finish that layout before publishing the action.
            suggestions.list.ScrollIntoView(suggestions.list.SelectedItem());
            suggestions.list.UpdateLayout();
        }
        // Deferred Enter can run in the same publication that creates its row.
        // Finish layout before reporting the row's actual viewport visibility.
        if (incoming == NativeShellCore::Activate) suggestions.list.UpdateLayout();
        if (incoming == NativeShellCore::Select || incoming == NativeShellCore::Activate) ReportVisibility();
        if (incoming == NativeShellCore::Activate || incoming == NativeShellCore::Fallback || incoming == NativeShellCore::Clipboard || incoming == NativeShellCore::ContextMenu || incoming == NativeShellCore::Blur) Edit();
        const uint32_t modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                                   ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                                   ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                                   ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
        const auto action = shell.NegotiateAddress(incoming, Observed(), modifiers);
        if (action.decision == NativeShellCore::Defer || action.decision == NativeShellCore::DropIntent) return;
        auto value = Message(kind);
        if (firefoxRequested) value.SetNamedValue(L"firefoxRequested", JValue::CreateBooleanValue(true));
        if (incoming == NativeShellCore::Select && traversal)
            value.SetNamedValue(L"traverse", JValue::CreateNumberValue(traversal));
        if (incoming == NativeShellCore::Fallback || incoming == NativeShellCore::Clipboard || incoming == NativeShellCore::ContextMenu || incoming == NativeShellCore::Blur) {
            value.SetNamedValue(L"editId", JValue::CreateNumberValue(double(shell.AddressState().edit_id)));
            value.SetNamedValue(L"text", JValue::CreateStringValue(field.Text()));
            value.SetNamedValue(L"inputType", JValue::CreateStringValue(observedInputType));
            value.SetNamedValue(L"textEpoch", JValue::CreateNumberValue(textEpoch));
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
            // Enter can supersede a queued arrow-clear before Firefox restores
            // the typed value. Carry that restoration in the same command.
            if (shell.AddressState().staged_kind == uint32_t(NativeShellCore::Select) && !pending.empty()) {
                const auto selection = JObject::Parse(pending);
                if (selection.HasKey(L"traverse") && selection.GetNamedString(L"id", L"").empty())
                    value.SetNamedValue(L"clearSelection", JValue::CreateBooleanValue(true));
            }
            JObject intent;
            intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((action.modifiers & 1) != 0));
            intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((action.modifiers & 2) != 0));
            intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((action.modifiers & 4) != 0));
            intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue((action.modifiers & 8) != 0));
            value.SetNamedValue(L"intent", intent);
        }
        int index = suggestions.CurrentSelectedIndex();
        if (incoming != NativeShellCore::ContextMenu && index >= 0 && size_t(index) < suggestions.ids.size())
            value.SetNamedValue(L"id", JValue::CreateStringValue(suggestions.ids[index]));
        Stage(value, incoming);
    }
    void ResultContext(wchar_t const* kind, winrt::hstring const& command = L"") {
        Edit();
        const auto negotiated = shell.NegotiateAddress(NativeShellCore::ContextMenu, Observed(), 0);
        if (negotiated.decision == NativeShellCore::Defer || negotiated.decision == NativeShellCore::DropIntent) return;
        auto value = Message(kind);
        value.SetNamedValue(L"resultId", JValue::CreateStringValue(resultMenuId));
        if (!resultMenuButtonId.empty()) value.SetNamedValue(L"buttonId", JValue::CreateStringValue(resultMenuButtonId));
        value.SetNamedValue(L"editId", JValue::CreateNumberValue(double(shell.AddressState().edit_id)));
        value.SetNamedValue(L"generation", JValue::CreateNumberValue(double(resultMenu.generation)));
        value.SetNamedValue(L"id", JValue::CreateStringValue(command));
        Stage(value, NativeShellCore::ContextMenu);
    }
    void ObserveFocus(bool islandHasFocus) {
        // FocusManager retains a logical target after Win32 focus leaves this
        // island. Only the owning pane's live island/popup focus can reveal it.
        islandFocusOwned = islandHasFocus;
        if (!islandHasFocus) firefoxFocusRequest = false;
        UpdateEmbeddedControls();
        // Popup focus can temporarily sit on the presenter, outside this root.
        // Retain Firefox's address edit target for the whole overflow opening.
        const bool overflowAddressMenu=navigation && navigation->overflowMenu.preserveAddress &&
            navigation->overflowMenu.flyout && navigation->overflowMenu.flyout.IsOpen();
        if ((mode.flyout.IsOpen() || context.IsOpen() || resultMenu.IsOpen() || pageActions.open || overflowAddressMenu) && islandHasFocus) return;
        bool inside = false;
        if (root.XamlRoot() && islandHasFocus) {
            auto focused =
                Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<DependencyObject>();
            while (focused) {
                // Keyboard traversal into the address's edit toolbar retains
                // that edit target and selection. Other toolbar buttons still
                // leave address focus normally.
                // Firefox identifies an address edit target in actual overflow.
                // Keep it while entering that menu, as for inline edit controls.
                if(navigation && navigation->overflowMenu.preserveAddress &&
                    (focused==navigation->buttons[6] || navigation->overflowMenu.Owns(focused)))inside=true;
                if(navigation)for(auto const& item:navigation->customization.state.items) {
                    auto row=navigation->stock.rows.find(item.id);
                    if(row==navigation->stock.rows.end())continue;
                    for(size_t i=0;i<item.stock.size();i++)if(item.stock[i].address &&
                        focused==row->second.buttons[i])inside=true;
                }
                if(inside)break;
                // Site/page buttons live inside the editor's visual border,
                // but focusing them must not start or restore an address query.
                if (focused == omnibarLeft.panel || focused == trailingActions) break;
                if (focused == root || (popupContent && focused == popupContent)) {
                    inside = true;
                    break;
                }
                focused = Media::VisualTreeHelper::GetParent(focused);
            }
        }
        ApplyInteraction(interaction.Observe(inside));
    }
    void LeaveForTraversal(int direction) {
        islandFocusOwned = false;
        UpdateEmbeddedControls();
        auto effect = interaction.Observe(false);
        effect.effects &= ~uint32_t(SHELL_ADDRESS_INTERACTION_EFFECT_BLUR);
        ApplyInteraction(effect);
        Action(L"blur", direction);
    }
    explicit NativeAddress(NativeShellCore& core) : mode(core), shell(core), interaction(core) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        root.Visibility(Visibility::Collapsed);
        root.Spacing(4);
        root.Margin({0, 0, 0, 12});
        field.PlaceholderText(L"Search or enter address");
        // An address is not prose: no Windows spell check or text prediction,
        // as Firefox's own URL bar has neither, in any window.
        field.IsSpellCheckEnabled(false);
        field.IsTextPredictionEnabled(false);
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
        Automation::AutomationProperties::SetAutomationId(field, L"FennAddress");
        field.GotFocus([this, alive=std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if(!alive.expired())formattedValue.Hide();
        });
        field.TextChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            formattedValue.Refresh(field, EditorOwned() || composing);
            Edit();
        });
        field.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (startupEdit) { if (!syncing) startupEdit(field.Text()); return; }
            formattedValue.Refresh(field, EditorOwned() || composing);
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
            value.SetNamedValue(L"editId", JValue::CreateNumberValue(double(shell.AddressState().edit_id)));
            value.SetNamedValue(L"text", JValue::CreateStringValue(field.Text()));
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
            nextInputType = L"insertCompositionText";
            Edit();
        });
        // Observe handled TextBox events without taking over native caret,
        // selection or capture. The core consumes each gesture exactly once.
        field.GettingFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
            if (!alive.expired() && e.FocusState() == FocusState::Pointer) interaction.PointerFocus();
        });
        field.AddHandler(UIElement::PointerPressedEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                    if (alive.expired()) return;
                    const auto point = e.GetCurrentPoint(field);
                    if (!point.Properties().IsLeftButtonPressed()) { interaction.Interrupt(EditorOwned()); return; }
                    firefoxFocusRequest = false;
                    interaction.Begin(point, !interaction.FocusWithin() || !islandFocusOwned);
                    if (preparePointerFocus) preparePointerFocus();
                    ScheduleFocusObservation();
                })), true);
        field.AddHandler(UIElement::PointerMovedEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                    if (!alive.expired()) interaction.Move(e.GetCurrentPoint(field));
                })), true);
        field.AddHandler(UIElement::PointerReleasedEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                    if (alive.expired()) return;
                    if (observeFocus) observeFocus();
                    ApplyInteraction(interaction.Release(e.GetCurrentPoint(field), EditorOwned()));
                    ScheduleFocusObservation();
                })), true);
        field.AddHandler(UIElement::PointerCanceledEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                    if (alive.expired()) return;
                    interaction.Cancel(e.Pointer().PointerId(), interaction.token);
                    ScheduleFocusObservation();
                })), true);
        field.AddHandler(UIElement::PointerCaptureLostEvent(),
            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
                    if (alive.expired()) return;
                    // Normal release may lose capture before PointerReleased.
                    // Reconcile next turn, using the old token so this cannot
                    // cancel a subsequent gesture or one in a new session.
                    const auto id=e.Pointer().PointerId(); const auto ticket=interaction.token;
                    auto queue=Dispatching::DispatcherQueue::GetForCurrentThread();
                    if (queue) queue.TryEnqueue([this,alive,id,ticket] {
                        if (alive.expired()) return;
                        interaction.Cancel(id,ticket);
                        ScheduleFocusObservation();
                    });
                })), true);
        field.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
        });
        field.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
            if (alive.expired()) return;
            if (!composing && e.Key() == winrt::Windows::System::VirtualKey::Enter)
                ApplyInteraction(interaction.PrepareSubmit(EditorOwned()));
            else interaction.Interrupt(EditorOwned());
            nextInputType = L"insertReplacementText";
            const auto key = uint32_t(e.Key());
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (NativeAltGrTextKey(key)) nextInputType = L"insertText";
            else if (key == VK_BACK) nextInputType = ctrl ? L"deleteWordBackward" : L"deleteContentBackward";
            else if (key == VK_DELETE) nextInputType = ctrl ? L"deleteWordForward" : L"deleteContentForward";
            else if (ctrl && key == 'Z') nextInputType = (GetKeyState(VK_SHIFT) & 0x8000) ? L"historyRedo" : L"historyUndo";
            else if (ctrl && key == 'Y') nextInputType = L"historyRedo";
            else if (!ctrl && !alt && ((key >= '0' && key <= 'Z') || key == VK_SPACE ||
                     (key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key >= VK_OEM_1 && key <= VK_OEM_3) ||
                     (key >= VK_OEM_4 && key <= VK_OEM_8) || key == VK_OEM_102)) nextInputType = L"insertText";
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
            if ((e.Key() == VirtualKey::Down || e.Key() == VirtualKey::Up) &&
                suggestions.HasCurrentRows() && !(GetKeyState(VK_MENU) & 0x8000)) {
                // Firefox publishes arrow destinations. Apply stock selection
                // synchronously so immediate Enter commits that exact row.
                const bool reverse = e.Key() == VirtualKey::Up;
                auto id = suggestions.ArrowTarget(reverse);
                syncing = true;
                suggestions.retainedSelection = -1;
                suggestions.SyncSelection(id);
                syncing = false;
                suggestions.RefreshLabels();
                Action(L"select", reverse ? -1 : 1);
                e.Handled(true);
            }
        });
        suggestions.list.SelectionChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            const auto index = suggestions.list.SelectedIndex();
            if (!syncing && index >= 0 && size_t(index) < suggestions.current.size() && !suggestions.current[index]) {
                syncing = true;
                suggestions.list.SelectedIndex(-1);
                syncing = false;
                return;
            }
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
        // The list's own effective viewport stays fixed while its internal
        // ScrollViewer moves rows. Observe that scroll as well as layout, so
        // newly revealed rows get exposure/activation IDs without polling.
        suggestions.list.Loaded([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            visibilityScroll.revoke();
            auto list = suggestions.list;
            list.ApplyTemplate();
            if (!Media::VisualTreeHelper::GetChildrenCount(list)) return;
            auto templateRoot = Media::VisualTreeHelper::GetChild(list, 0).try_as<FrameworkElement>();
            auto scroll = templateRoot ? templateRoot.FindName(L"ScrollViewer").try_as<ScrollViewer>() : nullptr;
            if (scroll) visibilityScroll = scroll.ViewChanged(winrt::auto_revoke,
                [this, alive](auto const&, auto const&) {
                    if (!alive.expired()) ScheduleVisibilityReport();
                });
            ScheduleVisibilityReport();
        });
        suggestions.list.Unloaded([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) visibilityScroll.revoke();
        });
        suggestions.list.EffectiveViewportChanged(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) ScheduleVisibilityReport();
            });
        suggestions.list.LayoutUpdated([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || !visibilityLayoutPending) return;
            visibilityLayoutPending = false;
            ReportVisibility();
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
                if (found && index < suggestions.current.size() && suggestions.current[index]) {
                    syncing = true;
                    list.SelectedIndex(int(index));
                    syncing = false;
                    Action(L"activate");
                }
            });
        resultMenu.snapshotKey = L"resultMenu";
        resultMenu.automationPrefix = L"FennResultContext-";
        resultMenu.focusEditor = false;
        resultMenu.action = [this, alive = std::weak_ptr<int>(lifetime)](auto kind, auto const& id) {
            if (!alive.expired()) ResultContext(kind, id);
        };
        suggestions.list.ContextRequested([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& e) {
            if (alive.expired()) return;
            auto source = e.OriginalSource().template try_as<DependencyObject>();
            ListViewItem row{nullptr};
            while (source && source != suggestions.list) {
                row = source.try_as<ListViewItem>();
                if (row) break;
                source = Media::VisualTreeHelper::GetParent(source);
            }
            winrt::Windows::Foundation::Point point{};
            const bool pointer = e.TryGetPosition(suggestions.list, point);
            if (!row && !pointer) row = suggestions.list.SelectedItem().try_as<ListViewItem>();
            uint32_t index{};
            if (!row || !suggestions.list.Items().IndexOf(row, index) || index >= suggestions.ids.size() || !suggestions.current[index]) return;
            e.Handled(true);
            resultMenuId = suggestions.ids[index];
            resultMenuButtonId = L"";
            resultMenu.anchor = row;
            resultMenu.requested = true;
            resultMenu.atPointer = e.TryGetPosition(row, resultMenu.position);
            ResultContext(L"context-open");
        });
        suggestions.list.PreviewKeyDown([this, alive = std::weak_ptr<int>(lifetime)](
                                            auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
            if (alive.expired()) return;
            using winrt::Windows::System::VirtualKey;
            if (e.Key() == VirtualKey::Down) {
                const auto next = suggestions.CurrentSelectedIndex() + 1;
                if (size_t(next) < suggestions.current.size() && !suggestions.current[next]) {
                    e.Handled(true); return;
                }
            }
            if (e.Key() == VirtualKey::Tab && (GetKeyState(VK_SHIFT) & 0x8000)) {
                auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(suggestions.list.XamlRoot());
                if (auto help = suggestions.FirstHelp(); help && focused == help) {
                    field.Focus(FocusState::Keyboard); e.Handled(true); return;
                }
            }
            if (e.Key() == VirtualKey::Enter) {
                auto source = e.OriginalSource().try_as<DependencyObject>();
                while (source && source != suggestions.list) {
                    if (source.try_as<Button>() || source.try_as<Documents::Hyperlink>()) return; // Let the stock button own Enter.
                    source = Media::VisualTreeHelper::GetParent(source);
                }
                Action(L"activate");
                e.Handled(true);
            }
            if (e.Key() == VirtualKey::Escape) {
                Action(L"dismiss");
                e.Handled(true);
                field.Focus(FocusState::Keyboard);
            }
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
            if (!alive.expired() && !composing && (GetKeyState(VK_MENU) & 0x8000) &&
                (e.Key() == winrt::Windows::System::VirtualKey::Down ||
                 e.Key() == winrt::Windows::System::VirtualKey::Up)) {
                mode.Queue(L"mode-reveal");
                e.Handled(true);
            }
        });
        header.Children().Append(mode.button);
        header.Children().Append(field);
        header.Children().Append(mode.exit);
        suggestions.openButtonMenu = [this](auto const& id, auto const& resultId, auto const& anchor) {
            resultMenuId = resultId; resultMenuButtonId = id;
            resultMenu.anchor = anchor; resultMenu.requested = true; resultMenu.atPointer = false;
            ResultContext(L"context-open");
        };
        suggestions.focusField = [this] { field.Focus(FocusState::Keyboard); };
        suggestions.activateButton = [this](auto const& id, auto const& resultId) { TryGlobalAction(id, resultId); };
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
                !(GetKeyState(VK_SHIFT) & 0x8000)) {
                if (popup && popup.IsOpen()) {
                    if (suggestions.FocusFirstAction()) {
                        e.Handled(true); return;
                    }
                }
                if (!actions.visuals.empty()) {
                    actions.visuals.front().button.Focus(FocusState::Keyboard); e.Handled(true);
                }
            }
        });
        root.Children().Append(header);
        fieldHeader = header;
        header.PointerEntered([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            UpdateEmbeddedControls();
        });
        for (auto leave : {&UIElement::PointerExitedEvent, &UIElement::PointerCanceledEvent}) {
            header.AddHandler((*leave)(), winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                    if (alive.expired()) return;
                    // Child exits bubble too, and cross-HWND exits can retain
                    // the last in-field event position. Read live coordinates.
                    UpdateEmbeddedControls();
                })), true);
        }
        header.GotFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
        });
        header.LostFocus([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) ScheduleFocusObservation();
        });
        mode.flyout.Closed([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) UpdateEmbeddedControls();
        });
        // Flatten the unfocused/hover border; leave the stock focused accent
        // and thickness intact. High contrast keeps its system borders.
        field.Resources(Markup::XamlReader::Load(LR"(
            <ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                                xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
                <ResourceDictionary.ThemeDictionaries>
                    <ResourceDictionary x:Key="Default">
                        <SolidColorBrush x:Key="TextControlBackground" Color="Transparent"/>
                        <StaticResource x:Key="TextControlBackgroundPointerOver" ResourceKey="SubtleFillColorSecondaryBrush"/>
                        <SolidColorBrush x:Key="TextControlBorderBrush" Color="Transparent"/>
                        <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="Transparent"/>
                    </ResourceDictionary>
                    <ResourceDictionary x:Key="Light">
                        <SolidColorBrush x:Key="TextControlBackground" Color="Transparent"/>
                        <StaticResource x:Key="TextControlBackgroundPointerOver" ResourceKey="SubtleFillColorSecondaryBrush"/>
                        <SolidColorBrush x:Key="TextControlBorderBrush" Color="Transparent"/>
                        <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="Transparent"/>
                    </ResourceDictionary>
                    <ResourceDictionary x:Key="HighContrast">
                        <SolidColorBrush x:Key="TextControlBorderBrush" Color="{ThemeResource SystemColorButtonTextColor}"/>
                        <SolidColorBrush x:Key="TextControlBorderBrushPointerOver" Color="{ThemeResource SystemColorHighlightColor}"/>
                    </ResourceDictionary>
                </ResourceDictionary.ThemeDictionaries>
            </ResourceDictionary>)").as<ResourceDictionary>());
        toolbarRoot = Grid();
        navigation = std::make_unique<NativeNavigationToolbar>();
        navigation->stock.activate=[this](wchar_t const* command,uint32_t modifiers,auto const& button,bool keyboard) {
            if(syncing || session.empty() || !navigate || navigation->customization.editing)return;
            if(navigation->forgetPanel.enabled)for(auto const& item:navigation->customization.state.items)
                if(item.placed && item.control==L"stock:panic-button" && winrt::hstring(command)==L"stock:"+item.id+L":panic-button") {
                    navigation->keyboardActivation=keyboard;navigation->forgetPanel.Open(button);return;
                }
            if(navigation->tabGroupsMenu.enabled)for(auto const& item:navigation->customization.state.items)
                if(item.placed && item.control==L"stock:tab-groups-button" && winrt::hstring(command)==L"stock:"+item.id+L":tab-groups-button") {
                    navigation->tabGroupsMenu.keyboardActivation=keyboard;navigation->tabGroupsMenu.Open(button);return;
                }
            if(navigation->profilerPanel.enabled)for(auto const& item:navigation->customization.state.items)
                if(item.placed && item.control==L"stock:profiler-button" && winrt::hstring(command)==L"stock:"+item.id+L":profiler-button") {
                    navigation->profilerPanel.keyboardActivation=keyboard;navigation->profilerPanel.Open(button);return;
                }
            if(navigation->developerMenu.enabled)for(auto const& item:navigation->customization.state.items)
                if(item.placed && item.control==L"stock:developer-button" && winrt::hstring(command)==L"stock:"+item.id+L":developer-button") {
                    navigation->developerMenu.keyboardActivation=keyboard;navigation->developerMenu.Open(button);return;
                }
            // Reuse the address clipboard contract while its editor owns focus;
            // Firefox's content command controller handles other edit targets.
            for(auto const& item:navigation->customization.state.items)if(item.placed)
                for(auto const& part:item.stock)if(part.address && part.enabled &&
                    winrt::hstring(command)==L"stock:"+item.id+L":"+part.id) {
                    if(part.id==L"cut-button"){Action(L"clipboard",0,L"cmd_cut");return;}
                    if(part.id==L"copy-button"){Action(L"clipboard",0,L"cmd_copy");return;}
                    if(part.id==L"paste-button"){Action(L"clipboard",0,L"cmd_paste");return;}
                }
            auto action=Message(L"navigation");
            action.SetNamedValue(L"command",JValue::CreateStringValue(command));
            action.SetNamedValue(L"keyboard",JValue::CreateBooleanValue(keyboard));
            action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(modifiers));
            auto local=button.TransformToVisual(nullptr).TransformBounds({0,0,float(button.ActualWidth()),float(button.ActualHeight())});
            auto screen=button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            for(auto const& item:navigation->customization.state.items)if(item.placed)
                for(auto const& part:item.stock)if(part.menu &&
                    winrt::hstring(command)==L"stock:"+item.id+L":"+part.id && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        navigation->activate = [this](wchar_t const* command, uint32_t modifiers, Controls::Button const& button) {
            if (syncing || session.empty() || !navigate) return;
            if (navigation->customization.editing && wcsncmp(command,L"customize:",10)!=0) return;
            if(wcsncmp(command,L"overflow-menu:invoke:",21)==0 || wcsncmp(command,L"overflow-menu:open:",19)==0)Edit();
            auto action = Message(L"navigation");
            if(wcsncmp(command,L"overflow-menu:invoke:",21)==0 || wcsncmp(command,L"overflow-menu:open:",19)==0) {
                auto editor=Message(L"clipboard");
                editor.SetNamedValue(L"editId",JValue::CreateNumberValue(double(shell.AddressState().edit_id)));
                editor.SetNamedValue(L"text",JValue::CreateStringValue(field.Text()));
                editor.SetNamedValue(L"textEpoch",JValue::CreateNumberValue(textEpoch));
                editor.SetNamedValue(L"selectionStart",JValue::CreateNumberValue(field.SelectionStart()));
                editor.SetNamedValue(L"selectionEnd",JValue::CreateNumberValue(field.SelectionStart()+field.SelectionLength()));
                editor.SetNamedValue(L"composing",JValue::CreateBooleanValue(composing));
                action.SetNamedValue(L"editor",editor);
            }
            action.SetNamedValue(L"command", JValue::CreateStringValue(command));
            action.SetNamedValue(L"keyboard", JValue::CreateBooleanValue(navigation->keyboardActivation));
            action.SetNamedValue(L"modifiers", JValue::CreateNumberValue(modifiers));
            if (wcscmp(command,L"indicator:private")==0 || wcscmp(command,L"indicator:analysis")==0 || wcsncmp(command, L"product:", 8) == 0 || wcscmp(command, L"back-history") == 0 || wcscmp(command, L"forward-history") == 0 || wcscmp(command, L"profile") == 0 || wcscmp(command, L"extensions") == 0 || wcscmp(command, L"appmenu") == 0 || wcscmp(command, L"overflow") == 0 || wcsncmp(command,L"profiler-panel:",15)==0 || wcsncmp(command,L"tab-groups-menu:",16)==0 || wcsncmp(command,L"privacy-panel:",14)==0 || wcsncmp(command,L"overflow-menu:",14)==0 || wcsncmp(command,L"profile-menu:",13)==0 || wcsncmp(command,L"downloads-panel:",16)==0 || wcscmp(command, L"downloads") == 0 || wcscmp(command, L"history") == 0 || wcsncmp(command,L"history-menu:open:",18)==0 || wcscmp(command, L"bookmarks") == 0 || wcscmp(command, L"reset-pbm") == 0 || wcsncmp(command, L"extension", 9) == 0 || wcsncmp(command,L"widget-menu:",12)==0 || wcsncmp(command, L"omni", 4) == 0) {
                auto local = button.TransformToVisual(nullptr).TransformBounds(
                    {0, 0, float(button.ActualWidth()), float(button.ActualHeight())});
                auto screen = button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
                action.SetNamedValue(L"x", JValue::CreateNumberValue(screen.X));
                action.SetNamedValue(L"y", JValue::CreateNumberValue(screen.Y));
                action.SetNamedValue(L"width", JValue::CreateNumberValue(screen.Width));
                action.SetNamedValue(L"height", JValue::CreateNumberValue(screen.Height));
                if (releaseMenuFocus && wcsncmp(command,L"profiler-panel:",15)!=0 && wcsncmp(command,L"tab-groups-menu:",16)!=0 && wcsncmp(command,L"privacy-panel:",14)!=0 && wcsncmp(command,L"overflow-menu:",14)!=0 && wcsncmp(command,L"profile-menu:",13)!=0 && wcsncmp(command,L"downloads-panel:",16)!=0 && wcsncmp(command,L"history-menu:",13)!=0 &&
                    wcsncmp(command,L"omni-pin:",9)!=0 && wcsncmp(command,L"omni-unpin:",11)!=0) releaseMenuFocus();
            }
            navigate(action.Stringify());
        };
        navigation->forgetPanel.core=shell.core;navigation->clearPrivatePanel.core=shell.core;
        navigation->forgetPanel.dispatch=[this](auto command,auto modifiers,auto const& button){navigation->activate(command,modifiers,button.template as<Controls::Button>());};
        navigation->clearPrivatePanel.dispatch=navigation->forgetPanel.dispatch;
        navigation->tabGroupsMenu.core=shell.core;
        navigation->tabGroupsMenu.dispatch=[this](auto command,auto modifiers,auto const& button){if(releaseMenuFocus && wcsncmp(command,L"tab-groups-menu:invoke:",23)==0)releaseMenuFocus();navigation->keyboardActivation=navigation->tabGroupsMenu.keyboardActivation;navigation->activate(command,modifiers,button.template as<Controls::Button>());};
        bookmarkPanel.core=shell.core;
        bookmarkPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto local=button.TransformToVisual(nullptr).TransformBounds({0,0,float(button.ActualWidth()),float(button.ActualHeight())});
            auto screen=button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"bookmark-panel:open:",20)!=0 && !bookmarkPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        permissionsPanel.core=shell.core;
        permissionsPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"permissions-panel:open:",23)!=0 && !permissionsPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        notificationPanel.core=shell.core;
        notificationPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"notification-panel:open:",24)!=0 && !notificationPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        connectionPanel.core=shell.core;
        connectionPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"connection-panel:open:",22)!=0 && !connectionPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        translationsPanel.core=shell.core;
        translationsPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"translation-panel:open:",23)!=0 && !translationsPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        selectedTranslationsPanel.core=shell.core;
        selectedTranslationsPanel.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"selected-translation-panel:open:",32)!=0 && !selectedTranslationsPanel.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        pageContextMenu.core=shell.core;
        pageContextMenu.dispatch=[this](auto const& payload,auto const& button){
            if(syncing || session.empty() || !navigate)return;
            auto action=Message(L"navigation");action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(0));for(auto const& value:payload)action.SetNamedValue(value.Key(),value.Value());
            auto source=button;if(!source.XamlRoot() || !source.IsLoaded())source=pageActions.button;
            auto local=source.TransformToVisual(nullptr).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            auto screen=source.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(wcsncmp(payload.GetNamedString(L"command").c_str(),L"page-context:open:",18)!=0 && !pageContextMenu.opening && releaseMenuFocus)releaseMenuFocus();
            navigate(action.Stringify());
        };
        navigation->profilerPanel.core=shell.core;
        navigation->profilerPanel.releaseFocus=[this]{if(releaseMenuFocus)releaseMenuFocus();};
        navigation->profilerPanel.dispatch=[this](auto command,auto modifiers,auto const& button){
            navigation->keyboardActivation=navigation->profilerPanel.keyboardActivation;
            navigation->activate(command,modifiers,button.template as<Controls::Button>());
        };
        navigation->developerMenu.core=shell.core;
        navigation->developerMenu.dispatch=[this](auto command,auto modifiers,auto const& button){
            auto action=Message(L"navigation");action.SetNamedValue(L"command",JValue::CreateStringValue(command));
            action.SetNamedValue(L"keyboard",JValue::CreateBooleanValue(navigation->developerMenu.keyboardActivation));
            action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(modifiers));
            auto local=button.TransformToVisual(nullptr).TransformBounds({0,0,float(button.ActualWidth()),float(button.ActualHeight())});
            auto screen=button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            navigate(action.Stringify());
        };
        navigation->backForwardMenu.core=shell.core;
        navigation->backForwardMenu.dispatch=[this](auto command,auto modifiers,auto const& button){
            auto action=Message(L"navigation");action.SetNamedValue(L"command",JValue::CreateStringValue(command));
            action.SetNamedValue(L"keyboard",JValue::CreateBooleanValue(navigation->backForwardMenu.keyboardActivation));
            action.SetNamedValue(L"modifiers",JValue::CreateNumberValue(modifiers));
            auto local=button.TransformToVisual(nullptr).TransformBounds({0,0,float(button.ActualWidth()),float(button.ActualHeight())});
            auto screen=button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
            action.SetNamedValue(L"x",JValue::CreateNumberValue(screen.X));action.SetNamedValue(L"y",JValue::CreateNumberValue(screen.Y));
            action.SetNamedValue(L"width",JValue::CreateNumberValue(screen.Width));action.SetNamedValue(L"height",JValue::CreateNumberValue(screen.Height));
            if(releaseMenuFocus && (wcsncmp(command,L"back-forward-menu:invoke:",25)==0 || wcsncmp(command,L"back-forward-menu:middle:",25)==0))releaseMenuFocus();
            navigate(action.Stringify());
        };
        navigation->historyMenu.core=shell.core;
        navigation->overflowMenu.core=shell.core;
        navigation->overflowMenu.dispatch=[this](auto command,auto modifiers,auto const& button){navigation->keyboardActivation=navigation->overflowMenu.keyboardActivation;navigation->activate(command,modifiers,button);};
        navigation->profileMenu.core=shell.core;
        navigation->profileMenu.dispatch=navigation->activate;
        navigation->downloadsPanel.core=shell.core;
        navigation->downloadsPanel.dispatch=navigation->activate;
        navigation->historyMenu.dispatch=navigation->activate;
        omnibarLeft.activate = navigation->activate;
        omnibarRight.activate = navigation->activate;
        pageActions.dispatch = navigation->activate;
        pageActions.changed = [this, alive = std::weak_ptr<int>(lifetime)] {
            if (!alive.expired()) UpdateEmbeddedControls();
        };
        pageActions.Create();
        for (auto* buttons : {&omnibarLeft, &omnibarRight}) {
            buttons->commandPrefix = L"omni:";
            buttons->contextPrefix = L"omni-menu:";
            buttons->automationPrefix = L"FennOmnibar-";
            buttons->buttonSize = 28;
            buttons->panel.Spacing(0);
            buttons->panel.VerticalAlignment(VerticalAlignment::Center);
        }
        omnibarLeft.panel.Margin({4, 0, 0, 0});
        omnibarRight.panel.Margin({0, 0, 0, 0});
        trailingActions.Orientation(Orientation::Horizontal);
        trailingActions.VerticalAlignment(VerticalAlignment::Center);
        trailingActions.Children().Append(omnibarRight.panel);
        trailingActions.Children().Append(pageActions.button);
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
        Grid::SetColumn(trailingActions, 4);
        // Keep logical tab order left-to-right while painting the editor
        // behind the embedded controls.
        Canvas::SetZIndex(field, -1);
        header.Children().Append(omnibarLeft.panel);
        header.Children().Append(mode.button);
        header.Children().Append(field);
        formattedValue.Create();
        Grid::SetColumnSpan(formattedValue.display, 5);
        header.Children().Append(formattedValue.display);
        header.Children().Append(mode.exit);
        header.Children().Append(trailingActions);
        mode.button.MinWidth(0); mode.button.MinHeight(0);
        mode.button.Height(24); mode.button.Margin({4, 0, 4, 0});
        // The stock 12-DIP arrow has internal side bearings; balance visible edge spacing.
        mode.button.Padding({8, 0, 6, 0});
        mode.button.VerticalAlignment(VerticalAlignment::Center);
        mode.button.BorderThickness({0, 0, 0, 0});
        // Keep the stock rectangular corners and dropdown template, with the
        // same themed interaction fills as the neighboring subtle buttons.
        mode.button.Resources(Markup::XamlReader::Load(LR"(
            <ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                                xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
                <ResourceDictionary.ThemeDictionaries>
                    <ResourceDictionary x:Key="Default">
                        <StaticResource x:Key="ButtonBackground" ResourceKey="SubtleButtonBackground"/>
                        <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SubtleButtonBackgroundPointerOver"/>
                        <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SubtleButtonBackgroundPressed"/>
                        <StaticResource x:Key="ButtonBackgroundDisabled" ResourceKey="SubtleButtonBackgroundDisabled"/>
                    </ResourceDictionary>
                    <ResourceDictionary x:Key="Light">
                        <StaticResource x:Key="ButtonBackground" ResourceKey="SubtleButtonBackground"/>
                        <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SubtleButtonBackgroundPointerOver"/>
                        <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SubtleButtonBackgroundPressed"/>
                        <StaticResource x:Key="ButtonBackgroundDisabled" ResourceKey="SubtleButtonBackgroundDisabled"/>
                    </ResourceDictionary>
                    <ResourceDictionary x:Key="HighContrast">
                        <StaticResource x:Key="ButtonBackground" ResourceKey="SubtleButtonBackground"/>
                        <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SubtleButtonBackgroundPointerOver"/>
                        <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SubtleButtonBackgroundPressed"/>
                        <StaticResource x:Key="ButtonBackgroundDisabled" ResourceKey="SubtleButtonBackgroundDisabled"/>
                    </ResourceDictionary>
                </ResourceDictionary.ThemeDictionaries>
            </ResourceDictionary>)").as<ResourceDictionary>());
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
                float(8 + width(mode.exit) + width(trailingActions)), 4});
            formattedValue.Refresh(field, EditorOwned() || composing);
        };
        for (auto element : {omnibarLeft.panel.as<FrameworkElement>(), mode.button.as<FrameworkElement>(),
                             mode.exit.as<FrameworkElement>(), trailingActions.as<FrameworkElement>()}) {
            element.SizeChanged(reserveControls);
            element.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), reserveControls);
        }
        // Match TabViewItem's live radius and its stock top-corner filter.
        // The square bottom edge joins the page below without a seam.
        toolbarSurface = Markup::XamlReader::Load(
            LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource LayerOnMicaBaseAltFillColorDefaultBrush}" CornerRadius="{Binding Source={ThemeResource OverlayCornerRadius}, Converter={StaticResource TopCornerRadiusFilterConverter}}"/>)").as<Grid>();
        toolbarSurface.Visibility(Visibility::Collapsed);

        toolbarRoot.Children().Append(toolbarSurface);
        toolbarRoot.Children().Append(root);
        navigation->Attach(toolbarRoot,root,toolbarSurface);
        root.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetAutomationId(toolbarRoot, L"FennNavigationToolbar");
        field.ContextFlyout(nullptr);
        field.SelectionFlyout(nullptr);
        context.editType = [this, alive = std::weak_ptr<int>(lifetime)](auto type) {
            if (!alive.expired()) nextInputType = type;
        };
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
    }
    void PlacePopup() {
        if (!popup || !root.XamlRoot()) return;
        popup.XamlRoot(root.XamlRoot());
        popupContent.Width(std::max(240.0, root.ActualWidth() - 10));
        popup.HorizontalOffset(0);
        // WinUI places the popup relative to the retained editor header.
        popup.VerticalOffset(0);
    }
    winrt::hstring Sync(JObject const& snapshot, bool retainEditor = false) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (snapshot.GetNamedNumber(L"version") != 1) winrt::throw_hresult(E_INVALIDARG);
        if (!snapshot.GetNamedBoolean(L"enabled", false)) {
            shell.ResetAddressPublication();
            shell.ResetToolbarPublication();
            if (navigation && !retainEditor) navigation->Apply({}, false);
            context.Reset();
            resultMenu.Reset();
            omnibarLeft.Apply({}, false); omnibarRight.Apply({}, false);
            pageActions.Apply({}, false);
            if (popup) popup.IsOpen(false);
            mode.Disable();
            actions.Clear();
            syncing = true;
            if (!retainEditor) root.Visibility(Visibility::Collapsed);
            suggestions.Clear();
            ResetSessionState();
            session = L"";
            syncing = false;
            return L"";
        }
        auto nextSession = snapshot.GetNamedString(L"session");
        auto checkedRevision = NativeJson::RequireRevision(snapshot, L"revision");
        auto checkedSequence = NativeJson::RequireRevision(snapshot, L"sequence");
        const auto checkedEdit = snapshot.HasKey(L"editId") ? NativeJson::RequireRevision(snapshot, L"editId") : 0;
        const auto checkedComplete = snapshot.GetNamedBoolean(L"complete", false);
        const auto checkedOpen = snapshot.GetNamedBoolean(L"open", false);
        const auto contentFocused = snapshot.GetNamedBoolean(L"contentFocused", false);
        const auto checkedRtl = snapshot.GetNamedBoolean(L"rtl", false);
        const auto checkedTextEpoch = snapshot.HasKey(L"textEpoch") ? NativeJson::RequireRevision(snapshot, L"textEpoch") : 0;
        const auto navigationToolbar = snapshot.GetNamedBoolean(L"navigationToolbar", false);
        auto customizationState = NativeToolbarCustomization::Read(snapshot);
        NativeStockToolbar::Validate(customizationState);
        const auto historyMenuState = NativeHistoryMenu::Read(snapshot);
        const auto downloadsPanelState = NativeDownloadsPanel::Read(snapshot);
        const auto profileMenuState = NativeProfileMenu::Read(snapshot);
        const auto overflowMenuState = NativeOverflowMenu::Read(snapshot);
        const auto profilerPanelState = NativeProfilerPanel::Read(snapshot);
        const auto bookmarkPanelState = NativeBookmarkPanel::Read(snapshot);
        const auto permissionsPanelState = NativeSitePermissionsPanel::Read(snapshot);
        const auto notificationPanelState = NativeNotificationPanel::Read(snapshot);
        const auto pageContextMenuState = NativePageContextMenu::Read(snapshot);
        const auto connectionPanelState = NativeConnectionPanel::Read(snapshot);
        const auto translationsPanelState = NativeTranslationsPanel::Read(snapshot);
        const auto selectedTranslationsPanelState = NativeSelectedTranslationsPanel::Read(snapshot);
        const auto developerMenuState = NativeDeveloperMenu::Read(snapshot);
        const auto backForwardMenuState = NativeBackForwardMenu::Read(snapshot);
        const auto tabGroupsMenuState = NativeTabGroupsMenu::Read(snapshot);
        const auto forgetPanelState = navigation ? navigation->forgetPanel.Read(snapshot) : JObject();
        const auto clearPrivatePanelState = navigation ? navigation->clearPrivatePanel.Read(snapshot) : JObject();
        const auto productMenuState = NativeProductMenu::Read(snapshot);
        const auto navigationState = NativeNavigationToolbar::Read(snapshot);
        const auto pinnedState = PinnedExtensionButtons::Read(snapshot);
        const auto paletteState = PinnedExtensionButtons::Read(snapshot,L"palette");
        const auto omnibarLeftState = PinnedExtensionButtons::Read(snapshot, L"omnibarLeft");
        const auto omnibarRightState = PinnedExtensionButtons::Read(snapshot, L"omnibarRight");
        const auto pageActionsState = NativePageActionsMenu::Read(snapshot);
        auto checkedText = snapshot.GetNamedString(L"text", L"");
        NativeAddressValue::Validate(snapshot);
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
        if (snapshot.HasKey(L"resultMenu")) {
            auto menu = snapshot.GetNamedObject(L"resultMenu");
            NativeJson::RequireRevision(menu, L"generation"); NativeJson::RequireRevision(menu, L"editId");
            NativeJson::RequireLength(menu.GetNamedString(L"resultId"), 64);
            size_t count = 0; NativeAddressContextMenu::Validate(menu.GetNamedArray(L"items"), count);
        }
        double completionSerial = 0, completionEdit = 0, completionStart = 0, completionEnd = 0;
        winrt::hstring completionBefore;
        if (snapshot.HasKey(L"autofill")) {
            const auto completion = snapshot.GetNamedObject(L"autofill");
            completionSerial = NativeJson::RequireRevision(completion, L"serial");
            completionEdit = NativeJson::RequireRevision(completion, L"editId");
            completionStart = NativeJson::RequireRevision(completion, L"start");
            completionEnd = NativeJson::RequireRevision(completion, L"end");
            completionBefore = completion.GetNamedString(L"before");
            if (completionBefore.size() > NativeJson::kMaxFieldText || completionEdit != checkedEdit ||
                completionStart > completionEnd || completionEnd > checkedText.size()) winrt::throw_hresult(E_INVALIDARG);
        }
        auto checkedRows = snapshot.GetNamedArray(L"rows");
        if (nextSession.empty() || nextSession.size() > 256 || checkedText.size() > NativeJson::kMaxFieldText ||
            checkedRows.Size() > NativeJson::kMaxListEntries)
            winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> checkedIds;
        size_t imagePixels = 0;
        if (snapshot.HasKey(L"arrowTargets")) {
            const auto targets = snapshot.GetNamedObject(L"arrowTargets");
            NativeJson::RequireLength(targets.GetNamedString(L"forward", L""), NativeJson::kMaxOpaqueId);
            NativeJson::RequireLength(targets.GetNamedString(L"backward", L""), NativeJson::kMaxOpaqueId);
            auto entries = targets.GetNamedArray(L"rows");
            if (entries.Size() > NativeJson::kMaxListEntries) winrt::throw_hresult(E_INVALIDARG);
            for (auto entry : entries) for (auto key : {L"id", L"forward", L"backward"})
                NativeJson::RequireLength(entry.GetObject().GetNamedString(key), NativeJson::kMaxOpaqueId);
        }
        for (auto value : checkedRows) {
            auto row = value.GetObject();
            auto id = row.GetNamedString(L"id");
            NativeJson::RequireLength(row.GetNamedString(L"presentationId", id), NativeJson::kMaxOpaqueId);
            if (id.empty() || id.size() > NativeJson::kMaxOpaqueId || !checkedIds.insert(id).second ||
                row.GetNamedString(L"title").size() > 4096 ||
                row.GetNamedString(L"description", L"").size() > 1024 ||
                row.GetNamedString(L"feedback", L"").size() > 1024 ||
                row.GetNamedString(L"displayUrl", L"").size() > 1024 ||
                row.GetNamedString(L"actionLabel", L"").size() > NativeJson::kMaxShortLabel)
                winrt::throw_hresult(E_INVALIDARG);
            SuggestionList::ValidateTextRuns(row, L"titleRuns", row.GetNamedString(L"title"));
            SuggestionList::ValidateTextRuns(row, L"urlRuns", row.GetNamedString(L"displayUrl", L""));
            if (row.HasKey(L"helpLink")) {
                auto help = row.GetNamedObject(L"helpLink");
                NativeJson::RequireLength(help.GetNamedString(L"id"), NativeJson::kMaxOpaqueId);
                for (auto key : {L"before", L"label", L"after"}) NativeJson::RequireLength(help.GetNamedString(key), 1024);
            }
            row.GetNamedBoolean(L"focusButton", false);
            row.GetNamedBoolean(L"enabled", true);
            row.GetNamedBoolean(L"stale", false);
            row.GetNamedBoolean(L"retainedSelected", false);
            row.GetNamedBoolean(L"titleLtr", false);
            row.GetNamedBoolean(L"inlineUrl", false);
            NativeJson::RequireLength(row.GetNamedString(L"symbol", L"page"), NativeJson::kMaxShortLabel);
            auto buttons = row.GetNamedArray(L"buttons", JsonArray());
            if (buttons.Size() > 8) winrt::throw_hresult(E_INVALIDARG);
            std::set<winrt::hstring> buttonIds;
            for (auto item : buttons) {
                auto button = item.GetObject(); auto id = button.GetNamedString(L"id");
                NativeJson::RequireLength(id, NativeJson::kMaxOpaqueId);
                NativeJson::RequireLength(button.GetNamedString(L"label"), 1024);
                if (id.empty() || !buttonIds.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
                button.GetNamedBoolean(L"enabled");
                button.GetNamedBoolean(L"menu", false);
                NativeJson::RequireLength(button.GetNamedString(L"menuLabel", L""), 1024);
            }
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
            modes.GetNamedBoolean(L"active");
            modes.GetNamedNumber(L"applied", 0);
            NativeJson::RequireLength(modes.GetNamedString(L"symbol", L"search"), NativeJson::kMaxShortLabel);
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
                entry.GetNamedBoolean(L"local", false);
                NativeJson::RequireLength(entry.GetNamedString(L"symbol", L"search"), NativeJson::kMaxShortLabel);
            }
        }
        // Validate the remaining presentation fields before classification or
        // any XAML mutation, including a replay or a late publication.
        NativeJson::RequireLength(snapshot.GetNamedString(L"placeholder", L"Search or enter address"), NativeJson::kMaxShortLabel);
        auto checkedLabels = snapshot.GetNamedObject(L"labels", JObject());
        for (auto key : {L"suggestions", L"actions", L"addressFallback"})
            NativeJson::RequireLength(checkedLabels.GetNamedString(key, L""), NativeJson::kMaxShortLabel);
        snapshot.GetNamedString(L"selectedId", L"");
        snapshot.GetNamedBoolean(L"modeButtonVisible", true);
        snapshot.GetNamedBoolean(L"modeExitVisible", false);
        snapshot.GetNamedNumber(L"modeReveal", 0);
        // The address entry point is a container: navigation/customization and
        // pinned state have independent identities. Fingerprint only this section.
        JObject addressSection;
        for (auto const& item : snapshot) {
            auto key = item.Key();
            if (key != L"navigation" && key != L"navigationToolbar" && key != L"version" && key != L"enabled" && key != L"startupBrowser")
                addressSection.SetNamedValue(key, item.Value());
        }
        auto addressFingerprint = addressSection.Stringify();
        const bool applyAddress = shell.AddressPublication(nextSession, uint64_t(checkedSequence),
            uint64_t(checkedRevision), addressFingerprint, false);
        const bool hasModes = snapshot.HasKey(L"modes") && snapshot.GetNamedValue(L"modes").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null;
        const auto modesObject = hasModes ? snapshot.GetNamedObject(L"modes") : JObject();
        const auto modesFingerprint = hasModes ? NativeSearchMode::Fingerprint(modesObject) : winrt::hstring();
        const auto modesGeneration = hasModes ? uint64_t(modesObject.GetNamedNumber(L"generation")) : 0;
        const auto modesDecision = applyAddress && hasModes
            ? shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_MODES, nextSession, modesGeneration,
                modesFingerprint, true, false) : SHELL_ADDRESS_SECTION_DECISION_IGNORED;
        const bool hasContext = snapshot.HasKey(L"contextMenu"), hasResult = snapshot.HasKey(L"resultMenu");
        const auto contextObject = hasContext ? snapshot.GetNamedObject(L"contextMenu") : JObject();
        const auto resultObject = hasResult ? snapshot.GetNamedObject(L"resultMenu") : JObject();
        const auto contextFingerprint = hasContext ? contextObject.Stringify() : winrt::hstring();
        const auto resultFingerprint = hasResult ? resultObject.Stringify() : winrt::hstring();
        const auto contextGeneration = hasContext ? uint64_t(contextObject.GetNamedNumber(L"generation")) : 0;
        const auto resultGeneration = hasResult ? uint64_t(resultObject.GetNamedNumber(L"generation")) : 0;
        const auto currentEdit = shell.AddressState().edit_id;
        const bool contextEligible = hasContext && shell.AddressMenuEligible(nextSession, session,
            uint64_t(contextObject.GetNamedNumber(L"editId")), currentEdit, L"", L"", context.requested, interaction.FocusWithin());
        const bool resultEligible = hasResult && shell.AddressMenuEligible(nextSession, session,
            uint64_t(resultObject.GetNamedNumber(L"editId")), currentEdit,
            resultObject.GetNamedString(L"resultId"), resultMenuId, resultMenu.requested, interaction.FocusWithin());
        const auto contextDecision = applyAddress && hasContext
            ? shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_CONTEXT_MENU, nextSession, contextGeneration,
                contextFingerprint, contextEligible, false) : SHELL_ADDRESS_SECTION_DECISION_IGNORED;
        const auto resultDecision = applyAddress && hasResult
            ? shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_RESULT_MENU, nextSession, resultGeneration,
                resultFingerprint, resultEligible, false) : SHELL_ADDRESS_SECTION_DECISION_IGNORED;
        const bool applyHistory = navigation && navigation->historyMenu.Resolve(historyMenuState);
        const bool applyOverflow = navigation && navigation->overflowMenu.Resolve(overflowMenuState);
        const bool applyBookmark = bookmarkPanel.Resolve(bookmarkPanelState);
        const bool applyPermissions = permissionsPanel.Resolve(permissionsPanelState);
        const bool applyNotification = notificationPanel.Resolve(notificationPanelState);
        const bool applyPageContextMenu = pageContextMenu.Resolve(pageContextMenuState);
        const bool applyConnection = connectionPanel.Resolve(connectionPanelState);
        const bool applyTranslations = translationsPanel.Resolve(translationsPanelState);
        const bool applySelectedTranslations = selectedTranslationsPanel.Resolve(selectedTranslationsPanelState);
        const bool applyProfiler = navigation && navigation->profilerPanel.Resolve(profilerPanelState);
        const bool applyBackForward = navigation && navigation->backForwardMenu.Resolve(backForwardMenuState);
        const bool applyDeveloper = navigation && navigation->developerMenu.Resolve(developerMenuState);
        const bool applyTabGroups = navigation && navigation->tabGroupsMenu.Resolve(tabGroupsMenuState);
        const bool applyForget = navigation && navigation->forgetPanel.Resolve(forgetPanelState);
        const bool applyClearPrivate = navigation && navigation->clearPrivatePanel.Resolve(clearPrivatePanelState);
        const bool applyProfile = navigation && navigation->profileMenu.Resolve(profileMenuState);
        const bool applyDownloads = navigation && navigation->downloadsPanel.Resolve(downloadsPanelState);
        const auto toolbarDecision = navigation && customizationState.revision
            ? shell.ToolbarPublication(customizationState.revision, customizationState.fingerprint, false)
            : SHELL_TOOLBAR_PUBLICATION_IGNORED;
        if(navigation && customizationState.revision && toolbarDecision!=SHELL_TOOLBAR_PUBLICATION_ACCEPTED) {
            auto labels=std::move(customizationState.labels);
            customizationState=navigation->customization.state;
            if(toolbarDecision==SHELL_TOOLBAR_PUBLICATION_UNCHANGED) customizationState.labels=std::move(labels);
        }
        if (applyAddress && nextSession != session) {
            pageActions.Close();
            // A queued layout report carries the old address session and can
            // be rejected after selection. Replay under the new session;
            // Firefox coalesces already-accepted identical overflow layouts.
            if (navigation) navigation->sentOverflow.clear();
            context.Reset();
            resultMenu.Reset();
            mode.Reset();
            clipboardSerial = 0;
            autofillSerial = 0;
            textEpoch = 0;
            ResetSessionState();
            session = nextSession;
            sequence = -1;
            visibility = L"";
            selectionId = 0;
            observedText = field.Text();
            observedComposing = composing;
        }
        if (applyAddress) {
            const auto flow = checkedRtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight;
            root.FlowDirection(flow);
            if (toolbarRoot) toolbarRoot.FlowDirection(flow);
            if (popupContent) popupContent.FlowDirection(flow);
            // Mirror the interface without reversing URL/text character order.
            // Reassigning editor formatting during an active TSF composition
            // can terminate it even when the requested value is unchanged.
            if (field.TextReadingOrder()!=TextReadingOrder::DetectFromContent)
                field.TextReadingOrder(TextReadingOrder::DetectFromContent);
            if (field.TextAlignment()!=TextAlignment::Left)
                field.TextAlignment(TextAlignment::Left);
            root.Visibility(Visibility::Visible);
        }
        if (toolbarRoot) {
            if(!customizationState.revision) shell.ResetToolbarPublication();
            else if(toolbarDecision==SHELL_TOOLBAR_PUBLICATION_ACCEPTED)
                shell.ToolbarPublication(customizationState.revision, customizationState.fingerprint, true);
            navigation->Apply(navigationState, navigationToolbar && snapshot.HasKey(L"navigation"));
            navigation->pinned.externalLayout = customizationState.revision != 0;
            navigation->pinned.Apply(pinnedState, navigationToolbar);
            navigation->historyMenu.Apply(historyMenuState,navigationToolbar && !navigation->customization.editing,navigation->buttons[9],applyHistory);
            navigation->productMenu.Apply(productMenuState,navigationToolbar);
            navigation->profileMenu.Apply(profileMenuState,navigationToolbar && !navigation->customization.editing,applyProfile);
            navigation->overflowMenu.Apply(overflowMenuState,navigationToolbar && !navigation->customization.editing,applyOverflow);
            navigation->profilerPanel.Apply(profilerPanelState,navigationToolbar && !navigation->customization.editing,applyProfiler);
            navigation->backForwardMenu.Apply(backForwardMenuState,navigationToolbar && !navigation->customization.editing,applyBackForward);
            navigation->developerMenu.Apply(developerMenuState,navigationToolbar && !navigation->customization.editing,applyDeveloper);
            navigation->tabGroupsMenu.Apply(tabGroupsMenuState,navigationToolbar && !navigation->customization.editing,applyTabGroups);
            navigation->forgetPanel.Apply(forgetPanelState,navigationToolbar && !navigation->customization.editing,applyForget);
            navigation->clearPrivatePanel.Apply(clearPrivatePanelState,navigationToolbar && !navigation->customization.editing,applyClearPrivate);
            navigation->downloadsPanel.Apply(downloadsPanelState,navigationToolbar && !navigation->customization.editing,applyDownloads);
            if (snapshot.HasKey(L"navigation")) {
                auto request = snapshot.GetNamedObject(L"navigation").GetNamedNumber(L"editorRequest", 0);
                if (std::isfinite(request) && request >= 0 && request <= NativeJson::kMaxSafeInteger) navigation->RequestEditor(request);
            }
            navigation->Layout(customizationState);
            navigation->UpdatePalette(paletteState);
            navigation->customization.Apply(customizationState, navigationToolbar);
            omnibarLeft.Apply(omnibarLeftState, navigationToolbar);
            omnibarRight.Apply(omnibarRightState, navigationToolbar);
            pageActions.Apply(pageActionsState, navigationToolbar && !navigation->customization.editing);
            auto bookmarkSource=omnibarRight.rows.find(bookmarkPanelState.GetNamedString(L"source",L""));
            bookmarkPanel.Apply(bookmarkPanelState,navigationToolbar && !navigation->customization.editing && bookmarkSource!=omnibarRight.rows.end(),bookmarkSource!=omnibarRight.rows.end()?bookmarkSource->second.button:pageActions.button,applyBookmark);
            auto permissionsSource=omnibarLeft.rows.find(permissionsPanelState.GetNamedString(L"source",L""));
            permissionsPanel.Apply(permissionsPanelState,navigationToolbar && !navigation->customization.editing,permissionsSource!=omnibarLeft.rows.end()?permissionsSource->second.button:pageActions.button,applyPermissions);
            auto notificationSource=omnibarLeft.rows.find(notificationPanelState.GetNamedString(L"source",L""));
            notificationPanel.Apply(notificationPanelState,navigationToolbar && !navigation->customization.editing,notificationSource!=omnibarLeft.rows.end()?notificationSource->second.button:pageActions.button,applyNotification);
            pageContextMenu.Apply(pageContextMenuState,navigationToolbar && !navigation->customization.editing,pageActions.button,applyPageContextMenu);
            selectedTranslationsPanel.Apply(selectedTranslationsPanelState,navigationToolbar && !navigation->customization.editing,pageActions.button,applySelectedTranslations);
            auto translationsSource=omnibarRight.rows.find(translationsPanelState.GetNamedString(L"source",L""));
            translationsPanel.Apply(translationsPanelState,navigationToolbar && !navigation->customization.editing,translationsSource!=omnibarRight.rows.end() && translationsSource->second.button.Visibility()==Visibility::Visible && translationsSource->second.button.ActualWidth()>0?translationsSource->second.button:pageActions.button,applyTranslations);
            auto connectionSource=omnibarLeft.rows.find(connectionPanelState.GetNamedString(L"source",L""));
            connectionPanel.Apply(connectionPanelState,navigationToolbar && !navigation->customization.editing,connectionSource!=omnibarLeft.rows.end()?connectionSource->second.button:pageActions.button,applyConnection);
            toolbarSurface.Visibility(navigationToolbar ? Visibility::Visible : Visibility::Collapsed);
            root.Margin(navigationToolbar ? Thickness{8, 4, 8, 4} : Thickness{0, 0, 0, 0});
            hideEmbeddedControls = navigationToolbar;
            UpdateEmbeddedControls();
        }
        if (!applyAddress) return L"";
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
        if (completionSerial && completionEdit != shell.AddressState().edit_id) return L"";
        shell.AddressPublication(nextSession, uint64_t(checkedSequence), uint64_t(checkedRevision), addressFingerprint, true);
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
            }
            const auto previousRevision = revision;
            revision = snapshot.GetNamedNumber(L"revision");
            textEpoch = double(checkedTextEpoch);
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
                } else if (completionSerial > autofillSerial && completionEdit == shell.AddressState().edit_id &&
                           field.Text() == completionBefore && std::wstring(text).starts_with(std::wstring(completionBefore))) {
                    field.Select(int(completionBefore.size()), 0);
                    field.SelectedText(winrt::hstring(std::wstring(text).substr(completionBefore.size())));
                } else field.Text(text);
                field.Select(int(text.size()), 0);
            }
            formattedValue.Apply(snapshot);
            formattedValue.Refresh(field, EditorOwned() || composing);
            if (clipboardId > clipboardSerial) {
                field.Select(int(clipboardStart), int(clipboardEnd - clipboardStart));
                clipboardSerial = double(clipboardId);
            }
            if (completionSerial > autofillSerial && completionEdit == shell.AddressState().edit_id) {
                field.Select(int(completionStart), int(completionEnd - completionStart));
                autofillSerial = completionSerial;
            }
            auto nextSequence = snapshot.GetNamedNumber(L"sequence", 0);
            if (sequence != nextSequence || previousRevision != revision) {
                sequence = nextSequence;
                // New containers can have no bounds until layout even when the
                // list's viewport and size are unchanged. Report after layout,
                // not just through size/viewport events or this synchronous pass.
                visibilityLayoutPending = true;
                suggestions.SyncRows(snapshot.GetNamedArray(L"rows"));
                visibility = L"";
            }
            const auto previousSelected = suggestions.CurrentSelectedIndex();
            suggestions.arrowTargets = snapshot.GetNamedObject(L"arrowTargets", JObject());
            suggestions.SyncSelection(snapshot.GetNamedString(L"selectedId", L""));
            if (suggestions.CurrentSelectedIndex() >= 0 && suggestions.CurrentSelectedIndex() != previousSelected)
                suggestions.list.ScrollIntoView(suggestions.list.SelectedItem());
            suggestions.RefreshLabels();
            if (modesDecision == SHELL_ADDRESS_SECTION_DECISION_ACCEPTED)
                shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_MODES, nextSession, modesGeneration, modesFingerprint, true, true);
            mode.Sync(snapshot, modesDecision != SHELL_ADDRESS_SECTION_DECISION_IGNORED);
            actions.Sync(snapshot);
            shell.PublishAddress(uint64_t(checkedEdit), checkedComplete || suggestions.HasCurrentRows());
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
        if (popup) {
            PlacePopup();
            popup.IsOpen(checkedOpen);
        }
        if (contextDecision == SHELL_ADDRESS_SECTION_DECISION_ACCEPTED)
            shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_CONTEXT_MENU, nextSession, contextGeneration, contextFingerprint, contextEligible, true);
        if (resultDecision == SHELL_ADDRESS_SECTION_DECISION_ACCEPTED)
            shell.AddressSection(SHELL_PUBLICATION_CHANNEL_ADDRESS_RESULT_MENU, nextSession, resultGeneration, resultFingerprint, resultEligible, true);
        context.Sync(snapshot, contextDecision == SHELL_ADDRESS_SECTION_DECISION_ACCEPTED, field);
        if (snapshot.HasKey(L"resultMenu") && snapshot.GetNamedObject(L"resultMenu").GetNamedString(L"resultId") != resultMenuId)
            resultMenu.Reset();
        else resultMenu.Sync(snapshot, resultDecision == SHELL_ADDRESS_SECTION_DECISION_ACCEPTED, field);
        if (interaction.FocusWithin() && !interaction.Holding() && (!popup || popup.IsOpen())) suggestions.FocusPending();
        // Firefox has already focused content. Finish only the HWND handoff,
        // after publication and pending-input guards; Escape and mode picks
        // leave Gecko focus on its address proxy and never take this path.
        // Gecko still reports content focus until a held entry gesture ends.
        // That older observation must not interrupt native text selection.
        if (!retainEditor && contentFocused && !checkedOpen && checkedEdit == shell.AddressState().edit_id)
            ApplyInteraction(shell.AddressInteraction(SHELL_ADDRESS_INTERACTION_KIND_CONTENT_FOCUS));
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
