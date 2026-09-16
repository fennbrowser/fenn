#pragma once
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include "JsonValidation.h"

// Presentation only. Firefox owns labels, eligibility and command meaning.
struct NativeTabMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Array = winrt::Windows::Data::Json::JsonArray;
    using Value = winrt::Windows::Data::Json::JsonValue;
    struct Entry {
        winrt::hstring kind, id, label, accessKey;
        bool enabled{};
        std::vector<Entry> children;
    };
    Microsoft::UI::Xaml::Controls::MenuFlyout active{nullptr};
    uint32_t tab{}, session{}, nextSession{}, revision{};
    uint32_t stateRevision{};
    winrt::hstring stateFingerprint;
    std::vector<std::vector<Entry>> models;
    std::map<uint32_t, uint32_t> tabModels;  // Only the current Firefox snapshot.
    std::deque<winrt::hstring> events;
    std::set<uint32_t> selectedTabs;
    bool hasSelection{};
    std::set<winrt::hstring> enabledIds;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    static Microsoft::UI::Xaml::Controls::MenuFlyoutItem Placeholder() {
        Microsoft::UI::Xaml::Controls::MenuFlyoutItem item;
        item.Text(L"\u2026");
        item.IsEnabled(false);
        return item;
    }

    static std::vector<Entry> Parse(Array const& entries, std::set<winrt::hstring>& ids, uint32_t& count,
                                    uint32_t depth = 0) {
        if (depth > 4 || entries.Size() > 128) throw winrt::hresult_invalid_argument();
        std::vector<Entry> result;
        for (auto value : entries) {
            if (++count > 128) throw winrt::hresult_invalid_argument();
            auto o = value.GetObject();
            Entry e;
            e.kind = o.GetNamedString(L"kind");
            if (e.kind != L"separator") {
                e.label = o.GetNamedString(L"label");
                e.accessKey = o.GetNamedString(L"accessKey", L"");
                e.enabled = o.GetNamedBoolean(L"enabled");
                if (e.label.empty() || e.label.size() > NativeJson::kMaxShortLabel || e.accessKey.size() > 16)
                    throw winrt::hresult_invalid_argument();
                if (e.kind == L"command") {
                    e.id = o.GetNamedString(L"id");
                    if (e.id.empty() || e.id.size() > 64 || !ids.insert(e.id).second)
                        throw winrt::hresult_invalid_argument();
                } else if (e.kind == L"submenu") {
                    e.children = Parse(o.GetNamedArray(L"items"), ids, count, depth + 1);
                    if (e.children.empty()) throw winrt::hresult_invalid_argument();
                } else
                    throw winrt::hresult_invalid_argument();
            }
            result.push_back(std::move(e));
        }
        return result;
    }
    void Queue(wchar_t const* kind, winrt::hstring const& id = L"") {
        // Bound input accumulation if the browser event loop is busy.
        if (events.size() >= 32) events.clear();
        Object event;
        event.SetNamedValue(L"kind", Value::CreateStringValue(kind));
        event.SetNamedValue(L"session", Value::CreateNumberValue(session));
        event.SetNamedValue(L"tab", Value::CreateNumberValue(tab));
        event.SetNamedValue(L"revision", Value::CreateNumberValue(revision));
        event.SetNamedValue(L"id", Value::CreateStringValue(id));
        events.push_back(event.Stringify());
    }
    void Dismiss(bool revoke = false) {
        if (revoke) events.clear();
        if (session) Queue(L"dismiss");
        session = tab = revision = 0;
        enabledIds.clear();
        auto old = active;
        active = nullptr;
        if (old && old.IsOpen()) old.Hide();
    }
    bool HasTab(uint32_t id) const { return tabModels.count(id) != 0; }
    void Close() {
        Dismiss();
        tabModels.clear();
        models.clear();
        events.clear();
        lifetime.reset();
    }

    void FocusFirst() {
        if (!active || !revision) return;
        for (auto item : active.Items()) {
            if (item.IsEnabled() && !item.try_as<Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator>()) {
                item.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
                break;
            }
        }
    }

    template <typename Vector>
    void Render(Vector const& target, std::vector<Entry> const& entries,
                Microsoft::UI::Xaml::Controls::MenuFlyout const& ownerMenu, uint32_t modelRevision,
                std::set<winrt::hstring>& modelIds, bool parentEnabled = true) {
        using namespace Microsoft::UI::Xaml::Controls;
        for (auto const& e : entries) {
            if (e.kind == L"separator") {
                target.Append(MenuFlyoutSeparator());
                continue;
            }
            if (e.kind == L"submenu") {
                MenuFlyoutSubItem item;
                item.Text(e.label);
                item.AccessKey(e.accessKey);
                item.IsEnabled(e.enabled);
                Render(item.Items(), e.children, ownerMenu, modelRevision, modelIds, parentEnabled && e.enabled);
                target.Append(item);
                continue;
            }
            MenuFlyoutItem item;
            item.Text(e.label);
            item.AccessKey(e.accessKey);
            item.IsEnabled(e.enabled);
            if (parentEnabled && e.enabled) modelIds.insert(e.id);
            auto expected = modelRevision;
            auto owner = winrt::make_weak(ownerMenu);
            item.Click(
                [this, alive = std::weak_ptr<int>(lifetime), owner, expected, id = e.id](auto const&, auto const&) {
                    if (alive.expired() || !session || active != owner.get() || revision != expected ||
                        !enabledIds.count(id))
                        return;
                    Queue(L"activate", id);
                    enabledIds.clear();  // A menu session can activate only once.
                });
            target.Append(item);
        }
    }
    void Populate(std::vector<Entry> const& entries) {
        auto rendered = winrt::single_threaded_vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase>();
        Render(rendered, entries, active, revision, enabledIds);
        active.Items().SetAt(0, rendered.GetAt(0));
        while (active.Items().Size() > 1) active.Items().RemoveAtEnd();
        for (uint32_t i = 1; i < rendered.Size(); ++i) active.Items().Append(rendered.GetAt(i));
    }
    void Open(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, uint32_t id) {
        Dismiss();
        if (!id || nextSession == UINT32_MAX) return;
        active = menu;
        tab = id;
        session = ++nextSession;
        // Firefox published this state before the row became interactive.
        // Keep the initial collection nonempty while replacing the placeholder.
        auto found = tabModels.find(id);
        if (found == tabModels.end()) {
            Dismiss(true);
            return;
        }
        revision = stateRevision;
        Populate(models[found->second - 1]);
        Queue(L"open");
    }
    winrt::hstring Sync(Object const& snapshot) {
        if (NativeJson::RequireId(snapshot, L"version") != 2) throw winrt::hresult_invalid_argument();
        auto publishOnly = snapshot.GetNamedBoolean(L"publishOnly", false);
        auto revoke = snapshot.HasKey(L"revoke") ? NativeJson::RequireId(snapshot, L"revoke") : 0;
        if (snapshot.HasKey(L"state")) {
            auto state = snapshot.GetNamedObject(L"state");
            auto incoming = NativeJson::RequireId(state, L"revision");
            auto descriptions = state.GetNamedArray(L"models");
            auto bindings = state.GetNamedArray(L"tabs");
            if (descriptions.Size() > NativeJson::kMaxMenuModels || bindings.Size() > NativeJson::kMaxTabs)
                throw winrt::hresult_invalid_argument();
            std::vector<std::vector<Entry>> nextModels;
            uint32_t total = 0;
            for (auto value : descriptions) {
                std::set<winrt::hstring> ids;
                uint32_t count = 0;
                auto entries = Parse(value.GetArray(), ids, count);
                total += count;
                if (entries.empty() || total > 4096) throw winrt::hresult_invalid_argument();
                nextModels.push_back(std::move(entries));
            }
            std::map<uint32_t, uint32_t> nextTabs;
            std::set<uint32_t> nextSelected;
            bool nextHasSelection = false;
            for (auto value : bindings) {
                auto binding = value.GetObject();
                auto id = NativeJson::RequireId(binding, L"tab");
                auto model = NativeJson::RequireId(binding, L"model");
                if (binding.HasKey(L"selected")) {
                    nextHasSelection = true;
                    if (binding.GetNamedBoolean(L"selected")) nextSelected.insert(id);
                }
                if (model > nextModels.size() || !nextTabs.emplace(id, model).second)
                    throw winrt::hresult_invalid_argument();
            }
            auto fingerprint = state.Stringify();
            if (incoming == stateRevision && fingerprint != stateFingerprint) throw winrt::hresult_invalid_argument();
            // Validate everything before mutation; never let a late state replace
            // the current window snapshot or alter an already displayed command.
            if (incoming > stateRevision) {
                models = std::move(nextModels);
                tabModels = std::move(nextTabs);
                selectedTabs = std::move(nextSelected);
                hasSelection = nextHasSelection;
                stateRevision = incoming;
                stateFingerprint = fingerprint;
                Dismiss(true);
            }
        }
        if (revoke && revoke == session) Dismiss(true);
        // Publishing before syncTabs must not accelerate action delivery.
        if (publishOnly || events.empty()) return L"";
        return events.front();  // Caller consumes only after copying into its buffer.
    }
};
