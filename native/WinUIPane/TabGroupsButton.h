#pragma once
#include <functional>
#include <winrt/Windows.Data.Json.h>
#include "JsonValidation.h"

// The tab-groups dropdown button and its collapse/expand flyout, presented
// above the tab strip. Firefox owns group membership and collapse state;
// this only renders the button/flyout and forwards a queued toggle. Fields
// this receives are already validated by the caller's upfront pass.
struct TabGroupsButton {
    Controls::DropDownButton button;
    hstring fingerprint, pending;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    // The owning strip's current presentation revision. Read live at click
    // time rather than captured when the flyout is built: the revision can
    // advance on any Publish that doesn't change groups (which skips
    // rebuilding the flyout), and a stale value here would make Firefox
    // reject an otherwise-legitimate toggle.
    std::function<uint32_t()> currentRevision;

    TabGroupsButton() {
        using namespace Microsoft::UI::Xaml;
        button.Visibility(Visibility::Collapsed);
        button.Margin({4, 0, 4, 0});
        button.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetAutomationId(button, L"FirefoxWinUITabGroups");
    }
    ~TabGroupsButton() { lifetime.reset(); }

    void Apply(hstring const& label, winrt::Windows::Data::Json::JsonArray const& groups) {
        using namespace Microsoft::UI::Xaml;
        using JObject = winrt::Windows::Data::Json::JsonObject;
        using JValue = winrt::Windows::Data::Json::JsonValue;
        // Group-only changes replace the flyout, never mutate a closing presenter.
        // Other state changes preserve the flyout but revoke any queued action.
        const auto groupState = groups.Stringify();
        if (groupState != fingerprint) {
            Controls::MenuFlyout flyout;
            for (auto value : groups) {
                auto group = value.GetObject();
                const auto id = NativeJson::RequireId(group, L"id");
                const bool collapsed = group.GetNamedBoolean(L"collapsed");
                Controls::ToggleMenuFlyoutItem item;
                item.Text(group.GetNamedString(L"name"));
                item.IsChecked(!collapsed);
                Automation::AutomationProperties::SetName(item, group.GetNamedString(L"label"));
                Automation::AutomationProperties::SetAutomationId(item, L"FirefoxWinUIGroup-" + to_hstring(id));
                item.Click([this, alive = std::weak_ptr<int>(lifetime), id, collapsed, groupState](auto const& sender, auto const&) {
                    if (alive.expired() || groupState != fingerprint) return;
                    sender.template as<Controls::ToggleMenuFlyoutItem>().IsChecked(!collapsed);
                    JObject action;
                    action.SetNamedValue(L"kind", JValue::CreateStringValue(L"strip-group"));
                    action.SetNamedValue(L"revision", JValue::CreateNumberValue(currentRevision()));
                    action.SetNamedValue(L"group", JValue::CreateNumberValue(id));
                    action.SetNamedValue(L"collapsed", JValue::CreateBooleanValue(!collapsed));
                    pending = action.Stringify();
                });
                flyout.Items().Append(item);
            }
            if (auto old = button.Flyout()) old.Hide();
            button.Flyout(flyout);
            fingerprint = groupState;
        }
        button.Content(box_value(label));
        button.Visibility(groups.Size() ? Visibility::Visible : Visibility::Collapsed);
        // Any successfully applied presentation revokes a queued toggle from
        // before it, whether or not the groups themselves changed.
        pending = L"";
    }
};
