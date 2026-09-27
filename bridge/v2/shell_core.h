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
#define SHELL_CORE_ABI_VERSION 20

#define MAX_EDIT_ID 9007199254740991

#define MAX_ROWS 10000

#define MAX_TRANSFERS 256

#define SHELL_NO_INDEX UINT32_MAX

#define MAX_TABS 10000

#define MAX_COMMANDS 128

#define MAX_ID 64

#define MAX_FINGERPRINT ((6 * 1024) * 1024)

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

/*
 Ordered group anchors (tab == 0) and visible members. Group zero is ungrouped.
 */
typedef struct {
  uint32_t group;
  uint32_t tab;
} ShellStripSlot;

typedef struct {
  uint32_t tab;
  uint32_t group;
  bool entering;
} ShellGroupMotionRow;

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

typedef struct {
  uint64_t endpoint;
  uint64_t gesture;
  uint64_t target;
  uint64_t target_endpoint;
  uint32_t tab;
  uint32_t target_revision;
  /*
   0 idle, 1 moving, 2 pending adoption, 3 consumed, 4 awaiting result, 5 continuation ready.
   */
  uint32_t phase;
} ShellWindowDrag;

typedef struct {
  uint64_t edit_id;
  uint32_t staged_kind;
  uint32_t needs_dispatch;
  uint32_t edit_acknowledged;
  uint32_t query_settled;
  uint32_t mode_pending;
} ShellAddressState;

typedef struct {
  /*
   An intent::Decision value.
   */
  uint32_t decision;
  /*
   Ctrl=1, Alt=2, Shift=4, Meta=8. Captured on the first Enter.
   */
  uint32_t modifiers;
} ShellAddressAction;

typedef struct {
  /*
   0 idle, 1 armed, 2 dragging.
   */
  uint32_t phase;
  uint32_t tab;
  uint32_t original;
  uint32_t projected;
  double start_x;
  double start_y;
  uint32_t detaching;
  uint32_t awaiting;
  uint64_t request;
  double landing_offset;
} ShellTabDrag;

typedef struct {
  double left;
  double width;
} ShellDragRow;

typedef struct {
  uint32_t session;
  uint32_t tab;
  uint32_t revision;
  uint32_t state_revision;
  uint32_t awaiting;
} ShellMenuState;

typedef struct {
  const uint16_t *data;
  uintptr_t len;
} ShellMenuCommand;

typedef struct {
  uint32_t active;
  uint32_t projected;
  double width;
  uint64_t request;
} ShellForeignPreview;

typedef struct {
  uint32_t id;
  double left;
  double width;
} ShellForeignRow;

typedef struct {
  uint64_t ticket;
  uint64_t source_endpoint;
  uint64_t source_token;
  uint64_t request;
  uint32_t tab;
  /*
   0 pending, 1 accepted (wait for publication), 2 rejected/cancelled.
   */
  uint32_t result;
} ShellTransfer;

typedef struct {
  uint32_t phase;
  uint32_t group;
  uint32_t before_group;
  uint32_t before_tab;
  bool valid;
  double marker;
  bool awaiting;
  uint64_t request;
} ShellGroupDrag;

typedef struct {
  uint32_t group;
  uint32_t tab;
  double start;
  double end;
} ShellGroupDragSlot;

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
 Observe the browser context revision and report whether native selection
 feedback is still pending. A mismatched revision permanently clears it.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_selection_pending(ShellCore *core, uint32_t revision, bool *out);

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
 Atomically validate strip metadata and optional ordered group layout.
 # Safety
 See [`guard`]. Arrays must contain their stated number of readable entries.
 */
ShellStatus shell_core_strip_apply_layout(ShellCore *core,
                                          uint32_t base,
                                          uint32_t revision,
                                          const ShellStripChange *changes,
                                          uintptr_t count,
                                          const ShellStripSlot *layout,
                                          uintptr_t layout_count,
                                          bool has_layout);

/*
 Plan visual departures/arrivals only for explicitly toggled groups.
 # Safety
 See [`guard`]. Arrays have their stated readable/writable lengths.
 */
ShellStatus shell_core_group_motion(ShellCore *core,
                                    const ShellStripSlot *old,
                                    uintptr_t old_count,
                                    const ShellStripSlot *new_,
                                    uintptr_t new_count,
                                    const uint32_t *groups,
                                    uintptr_t group_count,
                                    ShellGroupMotionRow *output,
                                    uintptr_t capacity,
                                    uintptr_t *count);

/*
 Resolve a tab/header hit to a different existing group; zero means no join.
 # Safety
 See [`guard`]. `group` must be writable.
 */
ShellStatus shell_core_strip_drop_group(ShellCore *core,
                                        uint32_t source,
                                        uint32_t tab,
                                        uint32_t header,
                                        uint32_t *group);

/*
 Copy the retained ordered layout. Returns required count; no partial copy.
 # Safety
 See [`guard`]. `count` must be writable; `slots` has `capacity` writable entries.
 */
ShellStatus shell_core_strip_layout(ShellCore *core,
                                    ShellStripSlot *slots,
                                    uintptr_t capacity,
                                    uintptr_t *count);

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

/*
 Observe an OS move. Operations: 0 query, 1 begin (target/revision are drag
 thresholds), 2 motion (eligible and destination identity), 3 finish (eligible
 means confirmed release or explicit hover handoff), 4 cancel, 5 take pending
 adoption by gesture token, 6 await request (gesture), 7 complete request
 (gesture/tab; zero tab rejects), 8 take the accepted continuation once.
 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_window_drag(ShellCore *core,
                                   uint32_t operation,
                                   uint32_t tab,
                                   int32_t x,
                                   int32_t y,
                                   uint64_t target,
                                   uint64_t target_endpoint,
                                   uint32_t revision,
                                   bool eligible,
                                   uint64_t gesture,
                                   ShellWindowDrag *out);

/*
 Observe a dwell candidate, or pass target=0 to cancel. Returns armed state.
 */
ShellStatus shell_core_group_hover(ShellCore *core,
                                   uint32_t source,
                                   uint32_t target,
                                   uint64_t now_ms,
                                   uint32_t delay_ms,
                                   bool *armed);

/*
 Read address coordination state; no WinUI objects or borrowed data escape.
 # Safety
 `core` is a live handle and `out` is writable.
 */
ShellStatus shell_core_address_state(ShellCore *core, ShellAddressState *out);

/*
 Reset on address disable or session replacement. Old outcomes cannot clear
 requests admitted after this reset because request IDs are endpoint-unique.
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_reset(ShellCore *core);

/*
 Forget the mode request on switcher disable/reset. This revokes local
 tracking, not browser work already admitted to the queue.
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_reset_mode(ShellCore *core);

/*
 Observe a new text/composition edit. Retry keeps this same edit identity.
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_edit(ShellCore *core);

/*
 Associate an admitted address request with its intent before draining it.
 Zero, non-address, drained and completed request IDs are rejected.
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_stage(ShellCore *core, uint64_t request, uint32_t kind);

/*
 Record the acknowledgment/readiness of an applied Firefox publication.
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_publish(ShellCore *core, uint64_t edit, bool settled);

/*
 Cancel retained Enter (mode changes and global actions).
 # Safety
 `core` is a live handle.
 */
ShellStatus shell_core_address_cancel_submit(ShellCore *core);

/*
 Negotiate an action and retain/consume deferred Enter modifiers. The core
 supplies mode priority, edit acknowledgment and query readiness; those fields in the
 caller's conditions are ignored here.
 # Safety
 `core` is live, `conditions` readable and `out` writable.
 */
ShellStatus shell_core_address_action(ShellCore *core,
                                      uint32_t incoming,
                                      const ShellIntentConditions *conditions,
                                      uint32_t modifiers,
                                      ShellAddressAction *out);

/*
 Consume the readiness notification for a retained Enter once per publication.
 # Safety
 `core` is live and `out` writable.
 */
ShellStatus shell_core_address_take_deferred(ShellCore *core, bool *out);

/*
 Read the local tab gesture. No control objects or borrowed geometry escape.
 # Safety
 Live core, writable out.
 */
ShellStatus shell_core_tab_drag_state(ShellCore *core, ShellTabDrag *out);

/*
 Arm a tab at a finite measured point, replacing previous local tracking.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_drag_arm(ShellCore *core, uint32_t tab, double x, double y);

/*
 Test the promotion threshold; a window-to-tab continuation bypasses distance.
 # Safety
 Live core, writable out.
 */
ShellStatus shell_core_tab_drag_ready(ShellCore *core,
                                      uint32_t tab,
                                      double x,
                                      double y,
                                      bool continuation,
                                      bool *out);

/*
 Copy at most 10,000 finite row rectangles when crossing the drag threshold.
 # Safety
 Live core; rows reads len aligned elements.
 */
ShellStatus shell_core_tab_drag_promote(ShellCore *core,
                                        uint32_t original,
                                        const ShellDragRow *rows,
                                        uintptr_t len);

/*
 Update attachment and optionally insertion projection. Group-hover presentation
 can inspect attachment first and suppress projection while it owns the gap.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_drag_update(ShellCore *core,
                                       double x,
                                       double y,
                                       double width,
                                       double height,
                                       bool project);

/*
 Project a validated existing-group join; tab zero appends at the header.
 # Safety
 Live core. Identities resolve against the accepted strip layout.
 */
ShellStatus shell_core_tab_drag_join(ShellCore *core, uint32_t group, uint32_t tab, bool before);

/*
 Hit existing-group members and the current insertion gap in preview geometry.
 # Safety
 Live core; writable outputs.
 */
ShellStatus shell_core_tab_drag_join_hit(ShellCore *core,
                                         double x,
                                         uint32_t header,
                                         bool collapsed,
                                         uint32_t *group,
                                         uint32_t *tab,
                                         bool *before);

/*
 Close the local reorder gap while retaining the active gesture.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_drag_close_gap(ShellCore *core);

/*
 Compute one sibling's offset from the frozen geometry.
 # Safety
 Live core, writable out.
 */
ShellStatus shell_core_tab_drag_offset(ShellCore *core, uint32_t index, double *out);

/*
 Offset a grouped-layout tab or a header anchored before index (len at end).
 # Safety
 Live core, writable out.
 */
ShellStatus shell_core_tab_drag_slot_offset(ShellCore *core,
                                            uint32_t index,
                                            bool header,
                                            bool source_header,
                                            bool join_header,
                                            double *out);

/*
 End the gesture; cancel also revokes pending reorder confirmation.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_drag_end(ShellCore *core, bool cancel);

/*
 Bind a queued reorder before draining. Zero denotes queue refusal/no request.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_drag_commit(ShellCore *core, uint64_t request, uint32_t before);

/*
 Test authoritative order confirmation, including disappearance of the source.
 # Safety
 Live core; ids reads len aligned IDs (null allowed only for zero len); writable out.
 */
ShellStatus shell_core_tab_drag_publish(ShellCore *core,
                                        const uint32_t *ids,
                                        uintptr_t len,
                                        bool *out);

/*
 Read menu identity and publication revision.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_menu_state(ShellCore *core, ShellMenuState *out);

/*
 Apply validated publication membership and exact serialized-state identity.
 # Safety
 Live core; readable bounded arrays (null only for length zero); writable changed.
 */
ShellStatus shell_core_menu_publish(ShellCore *core,
                                    uint32_t revision,
                                    const uint32_t *tabs,
                                    uintptr_t len,
                                    const uint16_t *fingerprint,
                                    uintptr_t units,
                                    bool *changed);

/*
 Retain only tabs still present in a native tab snapshot.
 # Safety
 Live core and readable IDs (null only for zero len).
 */
ShellStatus shell_core_menu_retain(ShellCore *core, const uint32_t *ids, uintptr_t len);

/*
 Test current published membership.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_menu_has_tab(ShellCore *core, uint32_t id, bool *out);

/*
 Start a new pending description; unknown tabs or exhausted session IDs refuse.
 # Safety
 Live core and writable opened.
 */
ShellStatus shell_core_menu_open(ShellCore *core, uint32_t tab, bool *opened);

/*
 Revoke the current session before hiding WinUI controls.
 # Safety
 Live core.
 */
ShellStatus shell_core_menu_dismiss(ShellCore *core);

/*
 Clear membership and session on teardown without reusing session IDs.
 # Safety
 Live core.
 */
ShellStatus shell_core_menu_close(ShellCore *core);

/*
 Bind one description and its enabled opaque command IDs to the current opening.
 # Safety
 Live core; readable commands and their UTF-16 slices; writable accepted.
 */
ShellStatus shell_core_menu_description(ShellCore *core,
                                        uint32_t session,
                                        uint32_t tab,
                                        uint32_t revision,
                                        const ShellMenuCommand *commands,
                                        uintptr_t len,
                                        bool *accepted);

/*
 Consume activation once for the exact session/revision and enabled opaque ID.
 # Safety
 Live core, readable ID and writable accepted.
 */
ShellStatus shell_core_menu_activate(ShellCore *core,
                                     uint32_t session,
                                     uint32_t revision,
                                     const uint16_t *id,
                                     uintptr_t len,
                                     bool *accepted);

/*
 Read preview state, including after cancellation/close for native cleanup.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_foreign_state(ShellCore *core, ShellForeignPreview *out);

/*
 Copy finite, bounded measured destination rows before starting a preview.
 # Safety
 Live core and readable rows (null allowed only for zero len).
 */
ShellStatus shell_core_foreign_begin(ShellCore *core, const ShellForeignRow *rows, uintptr_t len);

/*
 Project against frozen positions with the existing uniform gap-width rule.
 # Safety
 Live core and writable changed.
 */
ShellStatus shell_core_foreign_hover(ShellCore *core, double x, bool *changed);

/*
 Hover exit preserves a pending drop unless explicitly forced.
 # Safety
 Live core.
 */
ShellStatus shell_core_foreign_end(ShellCore *core, bool force);

/*
 Invalidate geometry on ordered membership change, returning whether to rehover.
 # Safety
 Live core, readable IDs (null only for zero len), writable refresh.
 */
ShellStatus shell_core_foreign_layout(ShellCore *core,
                                      const uint32_t *ids,
                                      uintptr_t len,
                                      bool *refresh);

/*
 Whole-window merge projection uses each measured row's own width.
 # Safety
 Live core, readable rows, writable index.
 */
ShellStatus shell_core_foreign_external_index(ShellCore *core,
                                              const ShellForeignRow *rows,
                                              uintptr_t len,
                                              double x,
                                              uint32_t *index);

/*
 Reserve durable destination tracking and an exact source departure token
 before hiding the source or admitting work. Both cores must share a thread;
 source==destination is supported for detach without aliased mutable borrows.
 # Safety
 Both handles live; writable out. No reentrant access during this call.
 */
ShellStatus shell_core_transfer_reserve(ShellCore *destination,
                                        ShellCore *source,
                                        uint32_t tab,
                                        ShellTransfer *out);

/*
 Bind only an admitted strip request before draining; zero records refusal.
 # Safety
 Live core. preview is true for ordinary destination drops, false for detach.
 */
ShellStatus shell_core_transfer_bind(ShellCore *core,
                                     uint64_t ticket,
                                     uint64_t request,
                                     bool preview);

/*
 Peek a durable cleanup decision. Repeated reads return it until acknowledged.
 # Safety
 Live core and writable out. Valid after close for destruction-time cleanup.
 */
ShellStatus shell_core_transfer_next(ShellCore *core, uint64_t after, ShellTransfer *out);

/*
 Release tracking capacity only after native effects have been applied.
 # Safety
 Live core.
 */
ShellStatus shell_core_transfer_ack(ShellCore *core, uint64_t ticket);

/*
 Cancel unresolved transfers when their native destination is destroyed.
 Already accepted outcomes remain accepted and still await source publication.
 # Safety
 Live core.
 */
ShellStatus shell_core_transfer_cancel(ShellCore *core);

/*
 Test a peer's exact endpoint and source departure token before UI restoration.
 If clear is true, clear only that matching token after the restoration succeeds.
 # Safety
 Live source core, writable matches.
 */
ShellStatus shell_core_departure_match(ShellCore *core,
                                       uint64_t endpoint,
                                       uint32_t tab,
                                       uint64_t token,
                                       bool clear,
                                       bool *matches);

/*
 Source removal confirms an accepted transfer and releases departure tracking.
 # Safety
 Live core.
 */
ShellStatus shell_core_departure_removed(ShellCore *core, uint32_t tab);

/*
 Read whole-group gesture state.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_group_drag_state(ShellCore *core, ShellGroupDrag *out);

/*
 Arm a group; IDs are group-scoped, never fake tab IDs.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_drag_arm(ShellCore *core, uint32_t group, double x, double y);

/*
 Advance a group using measured logical-axis blocks in browser order.
 # Safety
 Live core; slots reads len aligned entries, at most 10000.
 */
ShellStatus shell_core_group_drag_update(ShellCore *core,
                                         double x,
                                         double y,
                                         bool inside,
                                         double start,
                                         double end,
                                         const ShellGroupDragSlot *slots,
                                         uintptr_t len);

/*
 Logical-axis visual offset for a whole group or ungrouped-tab block.
 # Safety
 Live core; out is writable for one double.
 */
ShellStatus shell_core_group_drag_offset(ShellCore *core,
                                         uintptr_t index,
                                         double width,
                                         double *out);

/*
 Cancel or end the local group gesture before releasing native capture.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_drag_end(ShellCore *core);

/*
 Bind group selection stabilization to an admitted reorder request.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_drag_wait(ShellCore *core,
                                       uint64_t request,
                                       uint32_t group,
                                       uint32_t before_group,
                                       uint32_t before_tab);

/*
 Confirm whole-group order from a validated strip layout.
 # Safety
 Live core; slots reads len aligned entries, at most 20000.
 */
ShellStatus shell_core_group_drag_publish(ShellCore *core,
                                          const ShellStripSlot *slots,
                                          uintptr_t len);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* FIREFOX_WINUI_SHELL_CORE_H */
