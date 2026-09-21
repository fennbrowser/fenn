#pragma once
// Thin C++ wrapper over the project-owned Rust shell core (native/ShellCore).
//
// The Rust side owns scheduling, identity, bounds and the incremental tab
// model. This header owns nothing but the handle's lifetime and the mapping
// between Rust's batches and the C++ call sites, so the two languages meet at
// exactly one place.
//
// Ordering rule the whole file exists to enforce: a batch borrows memory the
// core owns, so every batch is released before any further core call, and the
// core is destroyed last. `Batch`/`Outcomes`/`Changes` below are move-only
// RAII types precisely so no call site can forget.
#include <stdint.h>

#include <functional>
#include <string_view>
#include <utility>

#include "shell_core.h"

struct NativeShellCore {
    // Request kinds, matching native/ShellCore/src/queue.rs. Kept as an enum
    // here rather than magic numbers at the call sites. The boundary carries
    // them as a plain uint32_t, so cbindgen does not generate the enum;
    // ffi.rs pins the accepted range so a kind added on one side alone fails
    // there.
    enum Kind : uint32_t {
        SelectTab = 1, MenuEvent = 2, CloseTab = 3, NewTab = 4, StripAction = 5, AddressAction = 6,
        AddressModeAction = 7, AddressVisibility = 8
    };
    // Outcomes, matching queue.rs `Outcome`.
    enum Outcome : uint32_t { Completed = 1, Rejected = 2, Cancelled = 3, Superseded = 4 };
    // Change kinds, matching model.rs `ChangeKind`.
    enum Change : uint32_t { TitleChanged = 1, SelectionChanged = 2, Inserted = 3, Removed = 4, Moved = 5 };
    // Address-bar intents and the decisions about them, matching intent.rs.
    // `NoIntent` is the boundary's "nothing is staged", not a kind.
    enum Intent : uint32_t {
        NoIntent = 0, Edit = 1, TextSelection = 2, Select = 3, Activate = 4,
        Focus = 5, Blur = 6, Dismiss = 7, GlobalAction = 8, Fallback = 9
    };
    enum Decision : uint32_t { Stage = 1, Replace = 2, Amend = 3, Defer = 4, DropIntent = 5 };

    // Which of an arriving intent and the staged one survives. The rules are
    // defined and exhaustively tested in intent.rs; this call is the only
    // definition the C++ side has, deliberately, so the two cannot drift.
    // Falls back to dropping the intent if the core is unavailable, which is
    // the safe direction: a dropped intent loses one action, a wrongly
    // accepted one can commit against the wrong suggestion.
    Decision Decide(Intent incoming, Intent staged, ShellIntentConditions const& conditions) const {
        uint32_t decision = DropIntent;
        if (!core) return DropIntent;
        if (shell_core_decide_intent(incoming, staged, &conditions, &decision) != SHELL_STATUS_OK)
            return DropIntent;
        return Decision(decision);
    }

    ShellCore* core{};

    NativeShellCore() = default;
    NativeShellCore(NativeShellCore const&) = delete;
    NativeShellCore& operator=(NativeShellCore const&) = delete;
    ~NativeShellCore() { Close(); }

    // Created on the owning STA, which is also Gecko's main thread; the core
    // refuses calls from anywhere else, so no unsafe Send declaration exists.
    bool Open(ShellCoreLimits const& limits) {
        if (core) return true;
        if (shell_core_abi_version() != SHELL_CORE_ABI_VERSION) return false;
        core = shell_core_create(&limits);
        return core != nullptr;
    }
    void Close() {
        if (!core) return;
        shell_core_destroy(core);
        core = nullptr;
    }
    explicit operator bool() const { return core != nullptr; }

    struct Batch {
        ShellRequestBatch raw{};
        Batch() = default;
        Batch(Batch const&) = delete;
        Batch& operator=(Batch const&) = delete;
        Batch(Batch&& other) noexcept : raw(other.raw) { other.raw = {}; }
        ~Batch() { shell_core_release_batch(&raw); }
        size_t size() const { return raw.len; }
        ShellRequest const& operator[](size_t index) const { return raw.requests[index]; }
        std::wstring_view payload(size_t index) const {
            auto const& request = raw.requests[index];
            return {reinterpret_cast<wchar_t const*>(request.payload), request.payload_len};
        }
    };
    struct Outcomes {
        ShellOutcomeBatch raw{};
        Outcomes() = default;
        Outcomes(Outcomes const&) = delete;
        Outcomes& operator=(Outcomes const&) = delete;
        Outcomes(Outcomes&& other) noexcept : raw(other.raw) { other.raw = {}; }
        ~Outcomes() { shell_core_release_outcomes(&raw); }
        size_t size() const { return raw.len; }
        ShellOutcome const& operator[](size_t index) const { return raw.outcomes[index]; }
    };
    struct Changes {
        ShellChangeBatch raw{};
        Changes() = default;
        Changes(Changes const&) = delete;
        Changes& operator=(Changes const&) = delete;
        Changes(Changes&& other) noexcept : raw(other.raw) { other.raw = {}; }
        ~Changes() { shell_core_release_changes(&raw); }
        size_t size() const { return raw.len; }
        ShellChange const& operator[](size_t index) const { return raw.changes[index]; }
    };

    // Returns the request id, or 0 when the core refused it. `notify` is set
    // only when the caller must now schedule exactly one drain notification.
    uint64_t Enqueue(Kind kind, uint32_t tab, uint32_t contextRevision, std::wstring_view payload,
                     bool& notify, uint32_t* status = nullptr) {
        notify = false;
        if (status) *status = 0;
        if (!core) return 0;
        ShellEnqueueResult result{};
        auto hr = shell_core_enqueue(core, kind, tab, contextRevision,
                                     reinterpret_cast<uint16_t const*>(payload.data()), payload.size(), &result);
        if (status) *status = result.status;
        notify = result.notify;
        return hr == SHELL_STATUS_OK ? result.id : 0;
    }

    bool NotificationFailed() {
        bool pending = false;
        if (core) shell_core_notification_failed(core, &pending);
        return pending;
    }

    bool BeginDrain() { return core && shell_core_begin_drain(core) == SHELL_STATUS_OK; }
    Batch TakeBatch(uint32_t maxEntries, size_t maxBytes) {
        Batch batch;
        if (core) shell_core_take_batch(core, maxEntries, maxBytes, &batch.raw);
        return batch;
    }
    bool EndDrain() {
        bool notify = false;
        if (core) shell_core_end_drain(core, &notify);
        return notify;
    }
    bool SetBlocked(bool blocked) {
        bool notify = false;
        if (core) shell_core_set_blocked(core, blocked, &notify);
        return notify;
    }
    bool Complete(uint64_t id, Outcome outcome) {
        return core && shell_core_complete(core, id, outcome) == SHELL_STATUS_OK;
    }
    Outcomes TakeOutcomes() {
        Outcomes outcomes;
        if (core) shell_core_take_outcomes(core, &outcomes.raw);
        return outcomes;
    }
    void CancelAll() {
        if (core) shell_core_cancel_all(core);
    }

    uint32_t ModelRevision() const {
        uint32_t revision = 0;
        if (core) shell_core_model_revision(core, &revision);
        return revision;
    }
    // SHELL_STATUS_GAP here means the producer's delta was computed against a
    // revision this core no longer holds: answer with one snapshot, never with
    // a retry loop.
    ShellStatus SetTitle(uint32_t base, uint32_t tab, std::wstring_view title, bool& changed) {
        changed = false;
        if (!core) return SHELL_STATUS_INVALID_ARGUMENT;
        return shell_core_model_set_title(core, base, tab, reinterpret_cast<uint16_t const*>(title.data()),
                                          title.size(), &changed);
    }
    ShellStatus SetSelected(uint32_t base, uint32_t tab, bool& changed) {
        changed = false;
        if (!core) return SHELL_STATUS_INVALID_ARGUMENT;
        return shell_core_model_set_selected(core, base, tab, &changed);
    }
    bool BeginSnapshot(uint64_t session, uint32_t capacity) {
        return core && shell_core_model_begin_snapshot(core, session, capacity) == SHELL_STATUS_OK;
    }
    bool PushSnapshotTab(uint32_t tab, std::wstring_view title) {
        return core && shell_core_model_push_snapshot_tab(
                           core, tab, reinterpret_cast<uint16_t const*>(title.data()), title.size()) ==
                           SHELL_STATUS_OK;
    }
    ShellStatus CommitSnapshot(uint32_t selected, uint32_t& changeCount) {
        changeCount = 0;
        if (!core) return SHELL_STATUS_INVALID_ARGUMENT;
        return shell_core_model_commit_snapshot(core, selected, &changeCount);
    }
    Changes TakeChanges() {
        Changes changes;
        if (core) shell_core_model_take_changes(core, &changes.raw);
        return changes;
    }
    bool NeedsSnapshot() const {
        bool value = false;
        if (core) shell_core_model_needs_snapshot(core, &value);
        return value;
    }
    uint32_t IndexOf(uint32_t tab) const {
        uint32_t index = SHELL_NO_INDEX;
        if (core) shell_core_model_index_of(core, tab, &index);
        return index;
    }
    ShellCoreStats Stats() const {
        ShellCoreStats stats{};
        if (core) shell_core_stats(core, &stats);
        return stats;
    }
};
