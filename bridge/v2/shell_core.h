/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* GENERATED FILE -- do not edit by hand.
 * Regenerate with scripts/Export-ShellCoreHeader.py (cbindgen 0.29.4).
 * The checked-in copy is what the native build compiles against; the script's
 * --check mode fails if this file and native/ShellCore/src/ffi.rs disagree. */

#ifndef FIREFOX_WINUI_SHELL_CORE_H
#define FIREFOX_WINUI_SHELL_CORE_H



#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/*
 Version of the C boundary in `bridge/v2/shell_core.h`. Bumped whenever the
 layout or meaning of anything in [`ffi`] changes; the native host refuses to
 use a core whose version it was not compiled against.
 */
#define SHELL_CORE_ABI_VERSION 2

#define SHELL_NO_INDEX UINT32_MAX

/*
 Result of every entry point. `Ok` is zero so the host can test it directly.
 */
enum ShellStatus
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_STATUS_OK = 0,
  /*
   A null pointer, an unknown enum value, or a length over a bound that has
   no more specific status.
   */
  SHELL_STATUS_INVALID_ARGUMENT = 1,
  /*
   Called from a thread other than the one that created this core.
   */
  SHELL_STATUS_WRONG_THREAD = 2,
  /*
   A drain is already running, or the queue is blocked.
   */
  SHELL_STATUS_BUSY = 3,
  /*
   `end_drain` without `begin_drain`.
   */
  SHELL_STATUS_NOT_DRAINING = 4,
  SHELL_STATUS_UNKNOWN_REQUEST = 5,
  /*
   This request already has an outcome: a replay inside the live session.
   */
  SHELL_STATUS_DUPLICATE_REQUEST = 6,
  /*
   A delta was computed against a revision this core no longer holds. The
   host answers with one snapshot.
   */
  SHELL_STATUS_GAP = 7,
  SHELL_STATUS_TOO_LARGE = 8,
  SHELL_STATUS_UNKNOWN_TAB = 9,
  /*
   The endpoint is closed; nothing new is accepted.
   */
  SHELL_STATUS_CLOSED = 10,
  /*
   A panic was contained. The operation did not happen and this core will
   refuse every later call.
   */
  SHELL_STATUS_PANIC = 11,
  /*
   The queue refused the request and said why in the result structure.
   */
  SHELL_STATUS_REJECTED = 12,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellStatus ShellStatus;
#else
typedef uint32_t ShellStatus;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Opaque per-endpoint state. One per native shell surface.
 */
typedef struct ShellCore ShellCore;

/*
 What the caller observed about the address surface when an intent arrived.
 Each field is `0` for false and non-zero for true; `bool` is deliberately
 not used across the boundary, where its representation is the host
 compiler's business.
 */
typedef struct {
  uint32_t applying;
  uint32_t composing;
  uint32_t has_session;
  uint32_t mode_intent_staged;
  uint32_t edit_acknowledged;
  uint32_t query_settled;
} ShellIntentConditions;

/*
 Bounds for one endpoint. Zero in any field means "use the built-in default
 for that field", so the host can set only what it cares about.
 */
typedef struct {
  uint32_t max_entries;
  uint32_t max_queued_bytes;
  uint32_t max_request_bytes;
  uint32_t max_in_flight;
  uint32_t completion_history;
  uint32_t max_pending_outcomes;
  uint32_t max_tabs;
  uint32_t max_title_units;
  uint32_t max_changes;
  uint32_t reserved;
} ShellCoreLimits;

typedef struct {
  uint64_t id;
  /*
   The earlier queued request this one replaced, or 0. Its
   [`Outcome::Superseded`] is waiting in `shell_core_take_outcomes`.
   */
  uint64_t superseded;
  /*
   One of [`crate::queue::EnqueueStatus`] as a number.
   */
  uint32_t status;
  /*
   True only when the host must now schedule exactly one drain
   notification. Never true while one is already outstanding.
   */
  bool notify;
  uint8_t reserved[3];
} ShellEnqueueResult;

/*
 One drained request. `payload` points into the batch and stays valid until
 `shell_core_release_batch`; copy anything you keep.
 */
typedef struct {
  uint64_t id;
  const uint16_t *payload;
  uintptr_t payload_len;
  uint32_t kind;
  uint32_t tab;
  uint32_t context_revision;
  uint32_t reserved;
} ShellRequest;

/*
 Owned by this crate. Release with `shell_core_release_batch`, exactly once.
 */
typedef struct {
  const ShellRequest *requests;
  uintptr_t len;
  /*
   Opaque owner. The host must not inspect, copy or free it.
   */
  void *handle;
} ShellRequestBatch;

typedef struct {
  uint64_t id;
  uint32_t outcome;
  uint32_t reserved;
} ShellOutcome;

/*
 Owned by this crate. Release with `shell_core_release_outcomes`.
 */
typedef struct {
  const ShellOutcome *outcomes;
  uintptr_t len;
  void *handle;
} ShellOutcomeBatch;

/*
 Low-overhead counters for the measurement harness. Reading them costs
 nothing beyond the copy, so the host can sample them on a hot path.
 */
typedef struct {
  uint64_t generation;
  uint64_t enqueued;
  uint64_t drained;
  uint64_t notifications;
  uint64_t dropped_outcomes;
  uint32_t queued_entries;
  uint32_t queued_bytes;
  uint32_t in_flight;
  uint32_t drain_state;
  uint32_t model_revision;
  uint32_t model_tabs;
  uint32_t pending_changes;
  uint32_t reserved;
} ShellCoreStats;

typedef struct {
  uint32_t id;
  /*
   Total pixels in the resulting row, including an image retained by omission.
   */
  uint32_t pixels;
  bool remove;
  /*
   New rows must supply every required field, not a patch of an unknown ID.
   */
  bool complete;
} ShellStripChange;

typedef struct {
  uint32_t kind;
  uint32_t tab;
  uint32_t from;
  uint32_t to;
} ShellChange;

/*
 Owned by this crate. Release with `shell_core_release_changes`.
 */
typedef struct {
  const ShellChange *changes;
  uintptr_t len;
  void *handle;
} ShellChangeBatch;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 Version of this boundary. The host refuses a core whose version differs from
 the one its header was generated for.
 */
uint32_t shell_core_abi_version(void);

/*
 Decide what to do with an arriving address-bar intent, given the one already
 staged. `staged` is `0` when nothing is outstanding. `out_decision` receives
 a [`crate::intent::Decision`] value.

 This is a pure function of its arguments: no core handle, no thread
 affinity, no allocation. It exists so the policy has one definition, tested
 in `intent.rs`, rather than one in Rust and another in the caller.

 # Safety
 `conditions` and `out_decision` must be readable/writable pointers.
 */
ShellStatus shell_core_decide_intent(uint32_t incoming,
                                     uint32_t staged,
                                     const ShellIntentConditions *conditions,
                                     uint32_t *out_decision);

/*
 Create one endpoint's core, owned by the calling thread. Returns null only
 if `limits` is null. Release with `shell_core_destroy`.

 # Safety
 `limits` must be null or point to a readable [`ShellCoreLimits`].
 */
ShellCore *shell_core_create(const ShellCoreLimits *limits);

/*
 Destroy the core. Outstanding requests are cancelled first, so nothing is
 left believing it is still in flight. Safe to call with null. Must be called
 on the owning thread; calling it twice is undefined, as for any `free`.

 # Safety
 `core` must be null, or a pointer from `shell_core_create` that has not been
 destroyed.
 */
void shell_core_destroy(ShellCore *core);

/*
 # Safety
 See [`guard`]. `payload` must be valid for `payload_len` `u16` values, and
 `out` must be writable.
 */
ShellStatus shell_core_enqueue(ShellCore *core,
                               uint32_t kind,
                               uint32_t tab,
                               uint32_t context_revision,
                               const uint16_t *payload,
                               uintptr_t payload_len,
                               ShellEnqueueResult *out);

/*
 Report that the drain notification could not be scheduled. The core returns
 to a state where the next enqueue notifies again; `still_pending` says
 whether work is waiting, so the host can retry deliberately.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_notification_failed(ShellCore *core, bool *still_pending);

/*
 # Safety
 See [`guard`].
 */
ShellStatus shell_core_begin_drain(ShellCore *core);

/*
 Take up to `max_entries` requests, or `max_bytes` of payload, whichever
 comes first (always at least one entry, so a large payload cannot wedge the
 queue). The batch and every `payload` in it stay valid until
 `shell_core_release_batch`.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_take_batch(ShellCore *core,
                                  uint32_t max_entries,
                                  uintptr_t max_bytes,
                                  ShellRequestBatch *out);

/*
 Release a batch from `shell_core_take_batch`. Safe with a zeroed batch.

 # Safety
 `batch` must be null or point to a batch this crate produced and that has
 not already been released.
 */
void shell_core_release_batch(ShellRequestBatch *batch);

/*
 End the drain. `notify` is set when the host must schedule exactly one more
 drain notification because work arrived while this drain was running.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_end_drain(ShellCore *core, bool *notify);

/*
 Block or unblock execution, for a host that cannot run browser commands
 right now. Unblocking sets `notify` when work is waiting; nothing is posted
 while blocked.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_set_blocked(ShellCore *core, bool blocked, bool *notify);

/*
 Record the authoritative outcome of one drained request.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_complete(ShellCore *core, uint64_t id, uint32_t outcome);

/*
 Whether this request may still be executed for the current generation.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_is_in_flight(ShellCore *core, uint64_t id, bool *out);

/*
 Take the outcomes recorded since the last call, so the host can reconcile
 optimistic native feedback with what actually happened.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_take_outcomes(ShellCore *core, ShellOutcomeBatch *out);

/*
 # Safety
 `batch` must be null or a batch this crate produced, not already released.
 */
void shell_core_release_outcomes(ShellOutcomeBatch *batch);

/*
 Cancel everything outstanding and advance the endpoint generation. Used when
 the session is replaced; the core stays usable.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_cancel_all(ShellCore *core);

/*
 Stop accepting work and cancel what is outstanding. Idempotent.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_close(ShellCore *core);

/*
 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_stats(ShellCore *core, ShellCoreStats *out);

/*
 Atomically accept a strip transaction after native JSON field validation.
 Base zero replaces the retained state; other bases must match exactly.

 # Safety
 See [`guard`]. `changes` must contain `count` readable entries.
 */
ShellStatus shell_core_strip_apply(ShellCore *core,
                                   uint32_t base,
                                   uint32_t revision,
                                   const ShellStripChange *changes,
                                   uintptr_t count);

/*
 The model revision a delta must be computed against.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_model_revision(ShellCore *core, uint32_t *out);

/*
 Apply one tab's title. O(1); no other tab is visited. `changed` reports
 whether anything actually differs, so an unchanged title costs no native
 property assignment.

 # Safety
 See [`guard`]. `title` must be valid for `title_len` `u16` values.
 */
ShellStatus shell_core_model_set_title(ShellCore *core,
                                       uint32_t base_revision,
                                       uint32_t tab,
                                       const uint16_t *title,
                                       uintptr_t title_len,
                                       bool *changed);

/*
 Apply the active tab. O(1).

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_model_set_selected(ShellCore *core,
                                          uint32_t base_revision,
                                          uint32_t tab,
                                          bool *changed);

/*
 Start a snapshot for `session`. Snapshots are built entry by entry so no
 array of pointers crosses the boundary. A second `begin` discards an
 unfinished one.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_model_begin_snapshot(ShellCore *core, uint64_t session, uint32_t capacity);

/*
 # Safety
 See [`guard`]. `title` must be valid for `title_len` `u16` values.
 */
ShellStatus shell_core_model_push_snapshot_tab(ShellCore *core,
                                               uint32_t tab,
                                               const uint16_t *title,
                                               uintptr_t title_len);

/*
 Commit the snapshot and report how many changes it produced, so a caller can
 measure how much structural work a snapshot actually caused.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_model_commit_snapshot(ShellCore *core,
                                             uint32_t selected,
                                             uint32_t *change_count);

/*
 Take the accumulated presentation changes. The host applies exactly these to
 its controls; there is no full snapshot to diff.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_model_take_changes(ShellCore *core, ShellChangeBatch *out);

/*
 # Safety
 `batch` must be null or a batch this crate produced, not already released.
 */
void shell_core_release_changes(ShellChangeBatch *batch);

/*
 Whether the consumer has fallen behind and the producer should send one
 snapshot. Never set by ordinary operation.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_model_needs_snapshot(ShellCore *core, bool *out);

/*
 Position of a tab in the current order, or `UINT32_MAX` if absent. O(1), so
 a single-tab update never searches the collection.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_model_index_of(ShellCore *core, uint32_t tab, uint32_t *out);

/*
 Deliberately panics, so the host's panic-containment path can be tested from
 a real call. Present only in the test build of the pane.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_test_panic(ShellCore *core);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* FIREFOX_WINUI_SHELL_CORE_H */
