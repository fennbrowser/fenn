#pragma once
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include <functional>
#include "JsonValidation.h"

// Presentation only. Firefox owns labels, eligibility and command meaning.
//
// Since ABI v11 a tab's command description is fetched on demand. The recurring
// publication carries tab identities and the selected set only -- the two things
// the list and the multiselection presentation actually need every time -- and a
// description is requested for exactly the tab whose menu is opening. Until that
// description arrives the menu shows a deliberately non-actionable pending row.
//
// The arriving description is bound to the opening request by session, tab and
// revision, so a completion that lands after dismissal, after the menu was
// retargeted to another row, or after teardown is discarded rather than
// rendered. Whatever is displayed is always the description of the request that
// is still open.
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
    // Descriptions are deliberately not cached across openings. Firefox is the
    // only authority on what a tab's commands are, and a description can go
    // stale from things no publication carries (a tab muting itself, a
    // navigation changing duplicate eligibility). One opening asks once; that
    // is the whole cost, and it is paid for the one tab being opened.
    // Tabs in the current Firefox publication. Membership is what makes a row's
    // context menu attachable and a strip selection addressable; it no longer
    // implies that a description exists.
    std::set<uint32_t> publishedTabs;
    // Set while a menu is open with the pending row shown and its description
    // still outstanding. Nothing in that state is actionable.
    bool awaitingDescription{};
    // Wired by the owning pane to the bounded request queue in the Rust shell
    // core. Menu transitions are ordered and are never coalesced.
    std::function<void(uint32_t tabId, uint32_t contextRevision, winrt::hstring const& action)> dispatch;
    std::set<uint32_t> selectedTabs;
    bool hasSelection{};
    std::set<winrt::hstring> enabledIds;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    static Microsoft::UI::Xaml::Controls::MenuFlyoutItem Placeholder() {
        Microsoft::UI::Xaml::Controls::MenuFlyoutItem item;
        item.Text(L"…");
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
        if (!dispatch || !session) return;
        Object event;
        event.SetNamedValue(L"kind", Value::CreateStringValue(kind));
        event.SetNamedValue(L"session", Value::CreateNumberValue(session));
        event.SetNamedValue(L"tab", Value::CreateNumberValue(tab));
        event.SetNamedValue(L"revision", Value::CreateNumberValue(revision));
        event.SetNamedValue(L"id", Value::CreateStringValue(id));
        dispatch(tab, revision, event.Stringify());
    }
    // `revoke` marks the session as already invalid on the Firefox side. There
    // is nothing to clear locally: every queued event carries this session id,
    // and Firefox rejects events for a session it no longer considers active.
    void Dismiss(bool revoke = false) {
        (void)revoke;
        if (session) Queue(L"dismiss");
        session = tab = revision = 0;
        awaitingDescription = false;
        enabledIds.clear();
        auto old = active;
        active = nullptr;
        if (old && old.IsOpen()) old.Hide();
    }
    bool HasTab(uint32_t id) const { return publishedTabs.count(id) != 0; }
    void Close() {
        Dismiss();
        publishedTabs.clear();
        dispatch = nullptr;
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
                    if (alive.expired() || !session || awaitingDescription || active != owner.get() ||
                        revision != expected || !enabledIds.count(id))
                        return;
                    Queue(L"activate", id);
                    enabledIds.clear();  // A menu session can activate only once.
                });
            target.Append(item);
        }
    }
    template <typename Vector>
    void Replace(Vector const& rendered) {
        // The flyout's collection must stay nonempty while it is replaced, or
        // WinUI can fail to attach a presenter (see implementation-lessons).
        active.Items().SetAt(0, rendered.GetAt(0));
        while (active.Items().Size() > 1) active.Items().RemoveAtEnd();
        for (uint32_t i = 1; i < rendered.Size(); ++i) active.Items().Append(rendered.GetAt(i));
    }
    void Populate(std::vector<Entry> const& entries) {
        auto rendered = winrt::single_threaded_vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase>();
        Render(rendered, entries, active, revision, enabledIds);
        Replace(rendered);
    }
    // The pending state: one disabled row, no command ids registered, so there
    // is nothing for a click or a keyboard activation to reach. This is what an
    // opening menu shows until its authoritative description arrives.
    void PopulatePending() {
        enabledIds.clear();
        auto rendered = winrt::single_threaded_vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase>();
        rendered.Append(Placeholder());
        Replace(rendered);
    }
    void Open(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, uint32_t id) {
        Dismiss();
        if (!id || nextSession == UINT32_MAX) return;
        if (!publishedTabs.count(id)) {
            // Not a row Firefox currently publishes: nothing to describe.
            return;
        }
        active = menu;
        tab = id;
        session = ++nextSession;
        revision = stateRevision;
        PopulatePending();
        awaitingDescription = true;
        // Firefox revalidates and answers with this tab's description. It also
        // revokes the session outright if the opening no longer makes sense.
        Queue(L"open");
    }
    void Sync(Object const& snapshot) {
        if (NativeJson::RequireId(snapshot, L"version") != 2) throw winrt::hresult_invalid_argument();
        // Validate the whole envelope before anything is applied. The caller
        // reads publishOnly after this returns, so a malformed value has to be
        // rejected here or a bad message could still have mutated state.
        snapshot.GetNamedBoolean(L"publishOnly", false);
        auto revoke = snapshot.HasKey(L"revoke") ? NativeJson::RequireId(snapshot, L"revoke") : 0;
        if (snapshot.HasKey(L"state")) {
            auto state = snapshot.GetNamedObject(L"state");
            auto incoming = NativeJson::RequireId(state, L"revision");
            auto bindings = state.GetNamedArray(L"tabs");
            if (bindings.Size() > NativeJson::kMaxTabs) throw winrt::hresult_invalid_argument();
            std::set<uint32_t> nextTabs;
            std::set<uint32_t> nextSelected;
            bool nextHasSelection = false;
            for (auto value : bindings) {
                auto binding = value.GetObject();
                auto id = NativeJson::RequireId(binding, L"tab");
                if (binding.HasKey(L"selected")) {
                    nextHasSelection = true;
                    if (binding.GetNamedBoolean(L"selected")) nextSelected.insert(id);
                }
                if (!nextTabs.insert(id).second) throw winrt::hresult_invalid_argument();
            }
            auto fingerprint = state.Stringify();
            if (incoming == stateRevision && fingerprint != stateFingerprint) throw winrt::hresult_invalid_argument();
            // Validate everything before mutation; never let a late state replace
            // the current window snapshot or alter an already displayed command.
            if (incoming > stateRevision) {
                publishedTabs = std::move(nextTabs);
                selectedTabs = std::move(nextSelected);
                hasSelection = nextHasSelection;
                stateRevision = incoming;
                stateFingerprint = fingerprint;
                Dismiss(true);
            }
        }
        if (snapshot.HasKey(L"menu")) {
            auto answer = snapshot.GetNamedObject(L"menu");
            auto forSession = NativeJson::RequireId(answer, L"session");
            auto forTab = NativeJson::RequireId(answer, L"tab");
            auto forRevision = NativeJson::RequireId(answer, L"revision");
            std::set<winrt::hstring> ids;
            uint32_t count = 0;
            auto items = Parse(answer.GetNamedArray(L"items"), ids, count);
            if (items.empty()) throw winrt::hresult_invalid_argument();
            // Bind the answer to the request that is still open. A dismissed,
            // retargeted or superseded opening leaves the pending row as it is.
            if (session && awaitingDescription && active && forSession == session && forTab == tab &&
                forRevision == revision && revision == stateRevision) {
                Populate(items);
                awaitingDescription = false;
            }
        }
        if (revoke && revoke == session) Dismiss(true);
    }
};
