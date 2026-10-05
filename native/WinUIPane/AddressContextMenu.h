#pragma once
#include "JsonValidation.h"

// WinUI menu/control ownership only. Firefox supplies items and browser actions.
struct NativeAddressContextMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Array = winrt::Windows::Data::Json::JsonArray;
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    std::function<void(wchar_t const*, winrt::hstring const&)> action;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    uint64_t generation{};
    bool requested{}, invoking{}, atPointer{};
    winrt::Windows::Foundation::Point position{};
    ~NativeAddressContextMenu() { lifetime.reset(); if (flyout) flyout.Hide(); }
    bool IsOpen() const { return flyout && flyout.IsOpen(); }
    void Reset() { requested = false; if (flyout) flyout.Hide(); flyout = nullptr; generation = 0; }
    static void Validate(Array const& items, size_t& count, unsigned depth = 0) {
        if (depth > 4 || (count += items.Size()) > 128) winrt::throw_hresult(E_INVALIDARG);
        for (auto value : items) {
            auto item = value.GetObject();
            if (item.GetNamedBoolean(L"separator", false)) continue;
            NativeJson::RequireLength(item.GetNamedString(L"id"), 64);
            NativeJson::RequireLength(item.GetNamedString(L"label"), 4096);
            NativeJson::RequireLength(item.GetNamedString(L"accessKey", L""), 16);
            item.GetNamedBoolean(L"enabled");
            item.GetNamedBoolean(L"checked", false); item.GetNamedBoolean(L"checkable", false);
            if (item.HasKey(L"children")) Validate(item.GetNamedArray(L"children"), count, depth + 1);
        }
    }
    void Populate(Array const& items,
        winrt::Windows::Foundation::Collections::IVector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& target,
        Microsoft::UI::Xaml::Controls::TextBox const& field) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        for (auto value : items) {
            auto item = value.GetObject();
            if (item.GetNamedBoolean(L"separator", false)) { target.Append(MenuFlyoutSeparator()); continue; }
            const auto id = item.GetNamedString(L"id"), label = item.GetNamedString(L"label");
            if (item.HasKey(L"children")) {
                MenuFlyoutSubItem sub; sub.Text(label); sub.IsEnabled(item.GetNamedBoolean(L"enabled"));
                Populate(item.GetNamedArray(L"children"), sub.Items(), field); target.Append(sub); continue;
            }
            auto configure = [&](auto button) {
                button.Text(label); button.AccessKey(item.GetNamedString(L"accessKey", L""));
                bool enabled = item.GetNamedBoolean(L"enabled");
                if (id == L"cmd_undo") enabled = field.CanUndo();
                if (id == L"cmd_redo") enabled = field.CanRedo();
                if (id == L"cmd_delete") enabled = field.SelectionLength() > 0;
                button.IsEnabled(enabled);
                Automation::AutomationProperties::SetAutomationId(button, L"FirefoxWinUIAddressContext-" + id);
                button.Click([this, field, id, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (alive.expired()) return;
                    invoking = true;
                    field.Focus(FocusState::Programmatic);
                    if (id == L"cmd_undo") field.Undo();
                    else if (id == L"cmd_redo") field.Redo();
                    else if (id == L"cmd_cut") field.CutSelectionToClipboard();
                    else if (id == L"cmd_copy") field.CopySelectionToClipboard();
                    else if (id == L"cmd_paste") field.PasteFromClipboard();
                    else if (id == L"cmd_delete") field.SelectedText(L"");
                    else if (id == L"cmd_selectAll") field.SelectAll();
                    else if (action) action(L"context-action", id);
                    invoking = false;
                });
                target.Append(button);
            };
            if (item.GetNamedBoolean(L"checkable", false)) {
                ToggleMenuFlyoutItem button;
                button.IsChecked(item.GetNamedBoolean(L"checked", false));
                configure(button);
            } else configure(MenuFlyoutItem());
        }
    }
    void Sync(Object const& snapshot, uint64_t editId, bool ownsFocus,
              Microsoft::UI::Xaml::Controls::TextBox const& field) {
        if (!snapshot.HasKey(L"contextMenu")) {
            if (!requested && !invoking && IsOpen()) Reset();
            return;
        }
        auto menu = snapshot.GetNamedObject(L"contextMenu");
        auto next = uint64_t(NativeJson::RequireRevision(menu, L"generation"));
        if (next == generation || !requested || !ownsFocus || NativeJson::RequireRevision(menu, L"editId") != editId) return;
        requested = false;
        generation = next;
        flyout = Microsoft::UI::Xaml::Controls::MenuFlyout();
        Populate(menu.GetNamedArray(L"items"), flyout.Items(), field);
        flyout.OverlayInputPassThroughElement(field.XamlRoot().Content());
        flyout.Closed([this, next, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired() && next == generation && !invoking && action) action(L"context-close", L"");
        });
        if (atPointer) {
            Microsoft::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
            options.Position(position);
            flyout.ShowAt(field, options);
        } else flyout.ShowAt(field);
    }
};
