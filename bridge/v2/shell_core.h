/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* GENERATED FILE -- do not edit by hand.
 * Regenerate with scripts/Export-ShellCoreHeader.py (cbindgen 0.29.4).
 * The checked-in copy is what the native build compiles against; the script's
 * --check mode fails if this file and native/ShellCore/src/ffi.rs disagree. */

#ifndef FENN_SHELL_CORE_H
#define FENN_SHELL_CORE_H



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
#define SHELL_CORE_ABI_VERSION 99

#define MAX_EDIT_ID 9007199254740991

#define MAX_ROWS 10000

/*
 A settled group that grew at least this much re-catches its moved edge.
 */
#define RECATCH_GROWTH 8.0

#define RECATCH_EDGE 4.0

/*
 Bound on groups per call.
 */
#define MAX_GROUPS 10000

#define SHELL_GLOW_LIT 1

#define SHELL_GLOW_HALO_LIT 2

#define SHELL_GLOW_GONE 4

#define SHELL_GLOW_HALO_GONE 8

/*
 Bound on surfaces and layout slots per frame.
 */
#define MAX_ENTRIES 10000

#define MAX_TRANSFERS 256

#define SHELL_NO_INDEX UINT32_MAX

#define MAX_TABS 10000

#define MAX_COMMANDS 128

#define MAX_ID 64

#define MAX_FINGERPRINT ((6 * 1024) * 1024)

#define MAX_ACTIONS 256

#define MAX_DESCRIPTION 65536

#define MAX_TOOLBAR_FINGERPRINT ((6 * 1024) * 1024)

#define MAX_PREVIEW_FINGERPRINT (1024 * 1024)

#define MAX_ADDRESS_SECTION_FINGERPRINT ((6 * 1024) * 1024)

/*
 Bound on simultaneously settling elements.
 */
#define MAX_SETTLING 4096

#define SHELL_SETTLE_TAB 0

#define SHELL_SETTLE_HEADER 1

/*
 Shared progress clock for a WinUI exit/layout checkpoint sequence.
 */
#define SHELL_SETTLE_CLOCK 2

#define TAB_MOTION_MS 135

#define GROUP_MOTION_MS 150

#define SHELL_MOTION_REPOSITION_MS 167

#define SHELL_MOTION_ENTER_MS 250

#define SHELL_MOTION_EXIT_MS 125

#define SHELL_MOTION_FADE_MS 83

#define SHELL_MOTION_TRAVEL_DIP 6.0

#define SHELL_MOTION_MOVE_X1 0.55

#define SHELL_MOTION_MOVE_Y1 0.55

#define SHELL_MOTION_MOVE_X2 0.0

#define SHELL_MOTION_MOVE_Y2 1.0

#define PAGE_INPUT_MAX_TEXT 65536

#define PAGE_INPUT_MAX_EVENTS 128

/*
 The event kind that tells Gecko a publication was accepted (`revision`).
 */
#define ACCEPTED 7

#define STARTUP_MAX_TABS 32

#define STARTUP_MAX_TEXT 32768

#define STARTUP_MAX_SUBMISSIONS 64

#define STARTUP_MAX_UNITS 262144

#define STARTUP_BROWSER_ROWS 4096

#define MAGIC 20038

#define VERSION 1

#define HEADER_BYTES 40

#define BYTES 16

#define MAX_CLIP 64

/*
 Strip and address, plus the clip list's bound.
 */
#define MAX_CHROME 2



#define MAX_TITLE 32768

/*
 Channel and kind numbers for C. They mirror [`frame::Channel`] and
 [`frame::Lifetime`]; a test keeps them equal.
 */
#define SHELL_FRAME_CONTROL 0

#define SHELL_FRAME_LIFETIME 1

#define SHELL_FRAME_GEOMETRY 2

#define SHELL_FRAME_TABS 3

#define SHELL_FRAME_ADDRESS 4

#define SHELL_FRAME_TAB_MENU 5

#define SHELL_FRAME_FOCUS 6

#define SHELL_FRAME_WINDOW 7

#define SHELL_FRAME_REQUESTS 8

#define SHELL_FRAME_STARTUP 9

#define SHELL_FRAME_CLOSE 10

#define SHELL_FRAME_PAGE 11

#define SHELL_FRAME_LIFETIME_BIND 0

#define SHELL_FRAME_LIFETIME_DESTROY 1

#define SHELL_FRAME_LIFETIME_WAKEUP_ON 2

#define SHELL_FRAME_LIFETIME_WAKEUP_OFF 3

#define SHELL_FRAME_LIFETIME_INTRODUCE 4

#define SHELL_FRAME_LIFETIME_DESTROY_HOST 5

#define SHELL_FRAME_LIFETIME_CREATE 6

#define SHELL_FRAME_LIFETIME_BOUND 7

#define SHELL_FRAME_LIFETIME_PAGE_READY 8

#define SHELL_FRAME_LIFETIME_PROFILE 9

#define SHELL_FRAME_LIFETIME_CONFIRM 10

#define SHELL_FRAME_LIFETIME_PRESENTED 11

#define SHELL_FRAME_LIFETIME_RECOVERY_COMMIT 12

#define SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED 13

#define SHELL_FRAME_LIFETIME_RECOVERY_RESUMED 14

#define SHELL_FRAME_LIFETIME_RECOVERY_EMPTY 15

#define SHELL_FRAME_LIFETIME_RECOVERY_SUPPRESS 16

#define SHELL_FRAME_REFUSED_STRIP 0

#define SHELL_FRAME_REFUSED_ADDRESS 1

#define SHELL_FRAME_REFUSED_BIND 2

/*
 Keep credited frames plus return credits below each OS adapter route's slots.
 */
#define SHELL_PIPE_FLIGHT_FRAMES 4

#define SHELL_PIPE_FLIGHT_BYTES ((4 * 1024) * 1024)

#define SHELL_STARTUP_ROW_CAPACITY 4128

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

enum ShellLinkSend
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_LINK_SEND_QUEUED = 0,
  SHELL_LINK_SEND_COALESCED = 1,
  SHELL_LINK_SEND_OVERFLOW = 2,
  SHELL_LINK_SEND_CLOSED = 3,
  SHELL_LINK_SEND_INVALID = 4,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellLinkSend ShellLinkSend;
#else
typedef uint32_t ShellLinkSend;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

enum ShellBackdrop
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_BACKDROP_MICA = 0,
  SHELL_BACKDROP_MICA_ALT = 1,
  SHELL_BACKDROP_ACRYLIC = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellBackdrop ShellBackdrop;
#else
typedef uint32_t ShellBackdrop;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Where the host takes a string or image field of a resolved row from.
 */
enum ShellStripSource
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  /*
   The value this publication carries.
   */
  SHELL_STRIP_SOURCE_TAKE = 0,
  /*
   The value the host already retains for this row.
   */
  SHELL_STRIP_SOURCE_KEEP = 1,
  /*
   No value: an empty string, or no image.
   */
  SHELL_STRIP_SOURCE_EMPTY = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellStripSource ShellStripSource;
#else
typedef uint32_t ShellStripSource;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

typedef enum {
  SHELL_TOOLBAR_PUBLICATION_IGNORED = 0,
  /*
   Revisioned content is identical; independent labels may refresh.
   */
  SHELL_TOOLBAR_PUBLICATION_UNCHANGED = 1,
  SHELL_TOOLBAR_PUBLICATION_ACCEPTED = 2,
} ShellToolbarPublication;

enum ShellAddressSectionDecision
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_ADDRESS_SECTION_DECISION_IGNORED = 0,
  SHELL_ADDRESS_SECTION_DECISION_ACCEPTED = 1,
  SHELL_ADDRESS_SECTION_DECISION_UNCHANGED = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellAddressSectionDecision ShellAddressSectionDecision;
#else
typedef uint32_t ShellAddressSectionDecision;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

enum ShellAddressInteractionKind
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_ADDRESS_INTERACTION_KIND_INSPECT = 0,
  SHELL_ADDRESS_INTERACTION_KIND_OBSERVE = 1,
  SHELL_ADDRESS_INTERACTION_KIND_POINTER_FOCUS = 2,
  SHELL_ADDRESS_INTERACTION_KIND_BEGIN = 3,
  SHELL_ADDRESS_INTERACTION_KIND_MOVE = 4,
  SHELL_ADDRESS_INTERACTION_KIND_RELEASE = 5,
  SHELL_ADDRESS_INTERACTION_KIND_CANCEL = 6,
  SHELL_ADDRESS_INTERACTION_KIND_INTERRUPT = 7,
  SHELL_ADDRESS_INTERACTION_KIND_CONTENT_FOCUS = 8,
  SHELL_ADDRESS_INTERACTION_KIND_PREPARE_SUBMIT = 9,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellAddressInteractionKind ShellAddressInteractionKind;
#else
typedef uint32_t ShellAddressInteractionKind;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

enum ShellAddressInteractionEffect
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_ADDRESS_INTERACTION_EFFECT_CLEAR_SELECTION = 1,
  SHELL_ADDRESS_INTERACTION_EFFECT_FOCUS = 2,
  SHELL_ADDRESS_INTERACTION_EFFECT_BLUR = 4,
  SHELL_ADDRESS_INTERACTION_EFFECT_SELECT_ALL = 8,
  SHELL_ADDRESS_INTERACTION_EFFECT_RELEASE_CONTENT_FOCUS = 16,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellAddressInteractionEffect ShellAddressInteractionEffect;
#else
typedef uint32_t ShellAddressInteractionEffect;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Values of [`ShellStripImage::kind`].
 */
enum ShellStripImageKind
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_STRIP_IMAGE_KIND_ABSENT = 0,
  /*
   `"image": null`.
   */
  SHELL_STRIP_IMAGE_KIND_NULL = 1,
  SHELL_STRIP_IMAGE_KIND_SET = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellStripImageKind ShellStripImageKind;
#else
typedef uint32_t ShellStripImageKind;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Publication channels whose acceptance runs through the shared protocol in
 [`crate::publication`]. Passed to [`shell_core_publication_stats`] as a
 `u32`.
 */
enum ShellPublicationChannel
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_PUBLICATION_CHANNEL_STRIP = 0,
  SHELL_PUBLICATION_CHANNEL_TAB_MENU_STATE = 1,
  SHELL_PUBLICATION_CHANNEL_SURFACE_MENU = 2,
  SHELL_PUBLICATION_CHANNEL_ADDRESS_SESSION = 3,
  SHELL_PUBLICATION_CHANNEL_TOOLBAR_CUSTOMIZATION = 4,
  SHELL_PUBLICATION_CHANNEL_TAB_MODEL = 5,
  SHELL_PUBLICATION_CHANNEL_TAB_PREVIEW = 6,
  SHELL_PUBLICATION_CHANNEL_ADDRESS_MODES = 7,
  SHELL_PUBLICATION_CHANNEL_ADDRESS_CONTEXT_MENU = 8,
  SHELL_PUBLICATION_CHANNEL_ADDRESS_RESULT_MENU = 9,
  SHELL_PUBLICATION_CHANNEL_DOWNLOADS_PANEL = 10,
  SHELL_PUBLICATION_CHANNEL_PROFILE_MENU = 11,
  SHELL_PUBLICATION_CHANNEL_OVERFLOW_MENU = 12,
  SHELL_PUBLICATION_CHANNEL_DEVELOPER_MENU = 13,
  SHELL_PUBLICATION_CHANNEL_FORGET_PANEL = 14,
  SHELL_PUBLICATION_CHANNEL_CLEAR_PRIVATE_PANEL = 15,
  SHELL_PUBLICATION_CHANNEL_TAB_GROUPS_MENU = 16,
  SHELL_PUBLICATION_CHANNEL_GROUP_HEADER = 17,
  SHELL_PUBLICATION_CHANNEL_PROFILER_PANEL = 18,
  SHELL_PUBLICATION_CHANNEL_BOOKMARK_PANEL = 19,
  SHELL_PUBLICATION_CHANNEL_PERMISSIONS_PANEL = 20,
  SHELL_PUBLICATION_CHANNEL_CONNECTION_PANEL = 21,
  SHELL_PUBLICATION_CHANNEL_TRANSLATIONS_PANEL = 22,
  SHELL_PUBLICATION_CHANNEL_SELECTED_TRANSLATIONS_PANEL = 23,
  SHELL_PUBLICATION_CHANNEL_PAGE_CONTEXT_MENU = 24,
  SHELL_PUBLICATION_CHANNEL_BACK_FORWARD_MENU = 25,
  SHELL_PUBLICATION_CHANNEL_ALL_TABS_PANEL = 26,
  SHELL_PUBLICATION_CHANNEL_NOTIFICATION_PANEL = 27,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellPublicationChannel ShellPublicationChannel;
#else
typedef uint32_t ShellPublicationChannel;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Native privacy confirmation surfaces; browser operations remain in Firefox.
 */
enum ShellPrivacyPanelKind
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_PRIVACY_PANEL_KIND_FORGET = 1,
  SHELL_PRIVACY_PANEL_KIND_CLEAR_PRIVATE = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellPrivacyPanelKind ShellPrivacyPanelKind;
#else
typedef uint32_t ShellPrivacyPanelKind;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

enum ShellLayoutWork
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_LAYOUT_WORK_WIDTHS = 0,
  SHELL_LAYOUT_WORK_AMBIENT = 1,
  SHELL_LAYOUT_WORK_CAPTION = 2,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellLayoutWork ShellLayoutWork;
#else
typedef uint32_t ShellLayoutWork;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Window binding operations. No callbacks or HWND operations occur in Rust.
 */
enum ShellWindowHostOperation
#if defined(__cplusplus) || __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // defined(__cplusplus) || __STDC_VERSION__ >= 202311L
 {
  SHELL_WINDOW_HOST_OPERATION_INSPECT = 0,
  SHELL_WINDOW_HOST_OPERATION_BIND = 1,
  SHELL_WINDOW_HOST_OPERATION_REQUEST_CLOSE = 2,
  SHELL_WINDOW_HOST_OPERATION_APPROVE_CLOSE = 3,
  SHELL_WINDOW_HOST_OPERATION_CANCEL_CLOSE = 4,
  SHELL_WINDOW_HOST_OPERATION_CONTENT_DESTROYED = 5,
  SHELL_WINDOW_HOST_OPERATION_SHELL_DESTROYED = 6,
  SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP = 7,
  SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP = 8,
};
#ifndef __cplusplus
#if __STDC_VERSION__ >= 202311L
typedef enum ShellWindowHostOperation ShellWindowHostOperation;
#else
typedef uint32_t ShellWindowHostOperation;
#endif // __STDC_VERSION__ >= 202311L
#endif // __cplusplus

/*
 Opaque per-endpoint state. One per native shell surface.
 */
typedef struct ShellCore ShellCore;

typedef struct ShellLinkEnd ShellLinkEnd;

/*
 Caller serializes access; this byte queue has no thread-affine UI objects.
 */
typedef struct ShellPipeQueue ShellPipeQueue;

typedef struct ShellRecovery ShellRecovery;

/*
 Bounds for one link end. Zero in any field keeps the built-in default.
 */
typedef struct {
  uint32_t in_flight_frames;
  uint32_t in_flight_bytes;
  uint32_t outbox_frames;
  uint32_t outbox_bytes;
  uint32_t snapshot_keys;
} ShellLinkLimits;

/*
 One received frame. `payload` points into the buffer given to
 `shell_link_receive` and lives as long as that buffer.
 */
typedef struct {
  /*
   False for a credit return or a frame of a retired window.
   */
  bool delivered;
  uint16_t channel;
  uint16_t kind;
  uint32_t window;
  uint64_t revision;
  const uint8_t *payload;
  uintptr_t length;
} ShellFrameView;

typedef struct {
  uint64_t sent;
  uint64_t received;
  uint64_t coalesced;
  uint64_t overflowed;
  uint64_t dropped;
  uint64_t outbox;
  uint64_t in_flight;
} ShellLinkCounts;

/*
 A tab row to encode; `title` holds `title_length` UTF-16 units.
 */
typedef struct {
  uint32_t id;
  uint32_t commands;
  const uint16_t *title;
  uintptr_t title_length;
} ShellFrameTabIn;

/*
 A decoded tab row; `title` points into the frame payload.
 */
typedef struct {
  uint32_t id;
  uint32_t commands;
  const uint16_t *title;
  uintptr_t title_length;
} ShellFrameTab;

/*
 Once retired, a late reply cannot bind or revive the window.
 */
typedef struct {
  uint32_t phase;
  uint32_t serial;
  uint64_t shell;
} ShellPipeBinding;

typedef struct {
  uint64_t times[2];
  uint32_t count;
  uint32_t reserved;
} ShellRecoveryLedger;

typedef struct {
  ShellRecoveryLedger shell;
  ShellRecoveryLedger engine;
} ShellRecoveryHistory;

typedef struct {
  uint64_t episode;
  uint64_t available_at;
  uint32_t eligible;
} ShellRecoveryRetry;

/*
 Per-window readiness barrier. Evidence is supplied only by each resource owner.
 */
typedef struct {
  uint32_t generation;
  uint32_t window;
  uint32_t required;
  uint32_t observed;
  uint64_t deadline;
  uint64_t last_time;
  uint64_t remaining;
  uint32_t paused;
  /*
   0 pending, 1 ready, 2 expired/invalid.
   */
  uint32_t phase;
} ShellRecoveryReadiness;

typedef struct {
  uint64_t id;
  uint8_t key[16];
  uint32_t kind;
} ShellRecoveryWindow;

typedef struct {
  uint64_t survivor;
  uint64_t restored;
  uint32_t kind;
} ShellRecoveryWindowEffect;

typedef struct {
  uint32_t peer;
  uint32_t generation;
  uint16_t nonce[32];
} ShellReconnectDescriptor;

/*
 Firefox-published identity for one endpoint. Privacy: 0 unknown, 1 normal,
 2 private. A zero key means no confirmed normal session-store identity yet.
 */
typedef struct {
  uint64_t revision;
  uint32_t privacy;
  uint8_t key[16];
} ShellRecoveryWindowIdentity;

typedef struct {
  uint32_t presentation;
  uint64_t draft;
  uint64_t browser;
  bool selected;
} ShellStartupRow;

/*
 Command transfer envelope. kind: 0 idle, 1 create-new-tab, 2 submit, 3 close,
 4 select a restored editor, 5 return to a mapped local editor, 6 import a draft.
 Text is owned by the core until completion/destruction. Inspecting a pending
 request is not an execution acknowledgment. Lost bindings cannot issue work.
 */
typedef struct {
  uint64_t session;
  uint64_t generation;
  uint64_t request;
  uint64_t draft;
  uint64_t browser;
  uint64_t submission;
  uint64_t revision;
  uint32_t kind;
  uint32_t modifiers;
  uintptr_t mappings;
  bool lost;
  uint64_t editor_browser;
  bool adoption_ready;
  uint32_t selection_start;
  uint32_t selection_length;
  const uint16_t *text;
  uintptr_t length;
} ShellStartupTransfer;

/*
 Startup draft view. Text is borrowed until the next mutation of this core.
 draft/request identities are local only and must never be used as browser IDs.
 */
typedef struct {
  uint64_t selected;
  uint64_t tab;
  uint64_t revision;
  uint64_t request;
  uintptr_t tabs;
  uintptr_t pending;
  uint32_t modifiers;
  const uint16_t *text;
  uintptr_t length;
  uint32_t selection_start;
  uint32_t selection_length;
  bool composing;
} ShellStartupView;

typedef struct {
  uint64_t generation;
  uintptr_t shell;
  uintptr_t content;
  uint64_t close_request;
  /*
   0 unbound, 1 attached, 2 awaiting Firefox, 3 approved, 4 content gone,
   5 shell destroyed, 6 native startup awaiting content, 7 startup cancelled.
   An endpoint can prepare and bind only once; cancellation is terminal.
   */
  uint32_t phase;
} ShellWindowBinding;

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

/*
 Lifetime totals of what a stream did with the publications it received,
 bucketed by the kind's policy. Diagnostics only: nothing reads them to
 decide anything, and a reset does not clear them.
 */
typedef struct {
  uint64_t accepted;
  uint64_t unchanged;
  /*
   Late publications the kind ignores (never on a delta stream).
   */
  uint64_t ignored;
  /*
   Refused without mutation: invalid, conflicting, a gap, or late on a
   delta stream, including a body the surface itself refused.
   */
  uint64_t refused;
} ShellPublicationCounts;

/*
 A JSON number the host read without interpreting; `present` is false when
 the key was absent.
 */
typedef struct {
  bool present;
  double value;
} ShellStripNumber;

/*
 UTF-16 enumeration name; `data` may be null only for length zero.
 */
typedef struct {
  bool present;
  const uint16_t *data;
  uintptr_t len;
} ShellStripName;

typedef struct {
  bool present;
  bool value;
} ShellStripFlag;

/*
 Strip publication envelope as read from its JSON object. The host checks
 JSON types only; ranges, enumerations, defaults and retention are decided
 by [`shell_core_strip_resolve`] and [`shell_core_strip_publish`].
 */
typedef struct {
  double version;
  double revision;
  /*
   Read only for version 2, where it is required.
   */
  ShellStripNumber base;
  ShellStripNumber window_drag_session;
  ShellStripNumber grouping_delay;
  ShellStripName backdrop;
  ShellStripName group_reach;
  ShellStripFlag window_drag_private;
  ShellStripFlag group_glow;
  ShellStripFlag container_glow;
  ShellStripFlag container_dot;
  ShellStripFlag group_hover;
  ShellStripFlag group_halo;
  ShellStripFlag group_glow_animations;
  ShellStripFlag group_glow_fade_out;
  ShellStripFlag rtl;
} ShellStripEnvelope;

/*
 The resolved publication envelope. A full snapshot (base zero) takes the
 defaults for omitted options; a delta keeps the accepted values.
 */
typedef struct {
  uint32_t base;
  uint32_t revision;
  /*
   Zero when Firefox has no window drag session for this strip.
   */
  uint32_t window_drag_session;
  uint32_t grouping_delay;
  ShellBackdrop backdrop;
  /*
   DIP that group surfaces reach past their outer members.
   */
  float group_reach;
  bool window_drag_private;
  bool group_glow;
  bool container_glow;
  bool container_dot;
  bool group_hover;
  bool group_halo;
  bool group_glow_animations;
  bool group_glow_fade_out;
  bool rtl;
} ShellStripOptions;

/*
 Presence and length of a row text field; the host keeps the string.
 */
typedef struct {
  bool present;
  uintptr_t len;
} ShellStripText;

/*
 A row image as read from the publication: raw dimensions and the hex pixel
 string, readable for `len` units when `kind` is `Set`.
 */
typedef struct {
  uint32_t kind;
  double width;
  double height;
  const uint16_t *pixels;
  uintptr_t len;
} ShellStripImage;

/*
 One row as read from the publication's `rows`, with JSON types checked only.
 */
typedef struct {
  double id;
  ShellStripNumber flags;
  ShellStripNumber container_color;
  ShellStripFlag group_source;
  ShellStripFlag group_target;
  ShellStripText detail;
  ShellStripText status;
  ShellStripText container_name;
  ShellStripImage image;
} ShellStripRowInput;

/*
 Ordered group anchors (tab == 0) and visible members. Group zero is ungrouped.
 */
typedef struct {
  uint32_t group;
  uint32_t tab;
} ShellStripSlot;

/*
 The publication's row data. `removals` holds the raw `removed` numbers, read
 only for deltas. Each pointer may be null only with a zero count.
 */
typedef struct {
  const ShellStripRowInput *rows;
  uintptr_t row_count;
  const double *removals;
  uintptr_t removal_count;
  const ShellStripSlot *layout;
  uintptr_t layout_count;
  bool has_layout;
} ShellStripPublication;

/*
 One accepted row, in publication order.
 */
typedef struct {
  uint32_t id;
  uint32_t flags;
  uint32_t container_color;
  uint32_t image_width;
  uint32_t image_height;
  ShellStripSource detail;
  ShellStripSource status;
  ShellStripSource container_name;
  ShellStripSource image;
  bool group_source;
  bool group_target;
} ShellStripRow;

typedef struct {
  uint32_t tab;
  uint32_t group;
  bool entering;
} ShellGroupMotionRow;

/*
 One bounded, fully validated title/selection publication. No mutation is
 visible until all rows and the publication identity have been accepted.
 */
typedef struct {
  uint32_t tab;
  const uint16_t *title;
  uintptr_t title_len;
} ShellTabTitle;

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

/*
 One refresh's inputs. `frame_ms` is sampled once per rendered frame, so
 layout passes inside one frame see the same extension; `wall_ms` is
 current time, so a held join still expires when no frames render.
 */
typedef struct {
  double frame_ms;
  double wall_ms;
  /*
   Group whose last visible member the drag gap lies past, and the gap's
   width; zero otherwise.
   */
  uint32_t tail_group;
  float tail_width;
  /*
   The dragged tab's own expanded group while it is a member; zero otherwise.
   */
  uint32_t source_group;
  /*
   The dragged tab's held slot, in surface coordinates.
   */
  float drag_slot_x;
  float drag_slot_width;
  /*
   The pointer, in surface coordinates.
   */
  float pointer_x;
  /*
   The drag is detaching or targeting a join, so it cannot be returning
   into its source group.
   */
  bool source_released;
  bool dragging;
  uint32_t join_group;
  bool animate;
  bool right_to_left;
  /*
   How far surfaces reach past their outer members, and the separation
   kept between neighboring groups.
   */
  float reach;
  float separation;
} ShellGroupSurfaceFrame;

typedef struct {
  uint32_t group;
  float x;
  float y;
  float width;
  float height;
} ShellGroupSurface;

typedef struct {
  /*
   A held or easing surface still moves on its own.
   */
  bool following;
  /*
   A surface holds an empty drag slot that needs a face.
   */
  bool holding;
} ShellGroupSurfaceResult;

/*
 One glow-eligible group (glow on and colored) this refresh.
 */
typedef struct {
  uint32_t group;
  /*
   Laid out and inside the viewport.
   */
  bool shown;
} ShellGlowGroup;

/*
 What happened to one group's light this refresh: `SHELL_GLOW_*` flags.
 */
typedef struct {
  uint32_t group;
  uint32_t flags;
} ShellGlowChange;

/*
 A shown group's horizontal extent in scroll-content space.
 */
typedef struct {
  uint32_t group;
  float left;
  float right;
} ShellGlowExtent;

/*
 A settled group grew: re-catch the edges that moved.
 */
typedef struct {
  uint32_t group;
  bool left;
  bool right;
} ShellGlowRecatch;

/*
 One element's current composition offset and horizontal scale.
 */
typedef struct {
  bool active;
  float dx;
  float sx;
} ShellSettleSample;

typedef struct {
  uint64_t edit_id;
  uint32_t staged_kind;
  uint32_t needs_dispatch;
  uint32_t edit_acknowledged;
  uint32_t query_settled;
  uint32_t mode_pending;
  uint32_t recoverable_edit;
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
  /*
   The pointer has left the strip's bounded margin: release detaches the
   whole group into a new window instead of reordering it.
   */
  bool detaching;
} ShellGroupDrag;

typedef struct {
  uint32_t group;
  uint32_t tab;
  double start;
  double end;
} ShellGroupDragSlot;

/*
 A group leaving this strip for another window. It stays hidden from the
 release until Firefox publishes a layout without it, or the move fails.
 */
typedef struct {
  uint32_t group;
  /*
   This strip's own detach request, or 0 when another window's strip
   queued the adopt and reports a rejection itself.
   */
  uint64_t request;
  /*
   0 pending, 1 accepted (still waiting for removal), 2 failed: restore.
   */
  uint32_t result;
} ShellGroupDeparture;

/*
 Destination of another window's whole-group drag. Insertion is between
 whole blocks (an ungrouped tab or a group), never inside a group. After the
 drop the gap is held until Firefox rejects the adopt or publishes.
 */
typedef struct {
  bool active;
  /*
   Measured block the group lands before; the block count means the end.
   */
  uint32_t index;
  uint32_t before_group;
  uint32_t before_tab;
  uint64_t request;
  /*
   For a held request: 0 pending, 1 accepted, 2 rejected or cancelled.
   */
  uint32_t result;
} ShellForeignGroup;

typedef struct {
  /*
   0 button, 1 address, 2 flexible space, 3 three-button stock group.
   */
  uint32_t kind;
  /*
   0 retained, 1 general overflow, 2 Firefox Extensions panel.
   */
  uint32_t overflow;
} ShellToolbarSlot;

typedef struct {
  double left;
  double width;
} ShellToolbarDropSlot;

typedef struct {
  uint32_t order[59];
  uint32_t len;
} ShellToolbarPaletteOrder;

typedef struct {
  uint32_t order[56];
  uint32_t len;
} ShellToolbarPreview;

/*
 Borrowed native menu context. Firefox supplies publication identity; WinUI
 supplies the current opening, editor and target observations.
 */
typedef struct {
  const uint16_t *session;
  uintptr_t session_len;
  const uint16_t *current_session;
  uintptr_t current_session_len;
  uint64_t edit;
  uint64_t current_edit;
  const uint16_t *target;
  uintptr_t target_len;
  const uint16_t *expected_target;
  uintptr_t expected_target_len;
  bool requested;
  bool focused;
} ShellAddressMenuContext;

typedef struct {
  uint32_t id;
  double top;
  double height;
} ShellAllTabsRow;

typedef struct {
  uint64_t token;
  uint32_t effects;
  uint32_t focused;
  uint32_t holding;
} ShellAddressInteractionState;

typedef struct {
  int32_t width;
  int32_t height;
  uint32_t dpi;
  /*
   Zero omits the strip; otherwise the supported 32/40/48 DIP row height.
   */
  uint32_t strip_height;
  bool navigation;
  /*
   Frame around navigation/content, in DIPs; ignored without navigation.
   */
  uint32_t content_inset;
} ShellChromeLayoutInput;

typedef struct {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
} ShellChromeRect;

typedef struct {
  bool fits;
  ShellChromeRect strip;
  ShellChromeRect navigation;
  ShellChromeRect content;
} ShellChromeLayout;

/*
 Adapter-defined finite geometry values, in stable order for each consumer.
 Exact comparison preserves subpixel movement and DPI changes. Unused slots
 are zero. This is an observation, never an authoritative layout model.
 */
typedef struct {
  double values[16];
} ShellLayoutInputs;

/*
 Platform-neutral composition presentation, with Win32 COLORREF values.
 kind: raw, selected raw, converted, selected converted (1..4).
 flags: text/background/underline color and underline style (1/2/4/8).
 */
typedef struct {
  uint32_t start;
  uint32_t end;
  uint32_t kind;
  uint32_t flags;
  uint32_t line;
  uint32_t bold;
  uint32_t foreground;
  uint32_t background;
  uint32_t underline;
} ShellPageClause;

/*
 Native TSF projection operation. 0 inspect, 1 publish, 2 select, 3 replace,
 4 enqueue composition (kind 1 start, 2 update, 3 end), 5 consume front event.
 Buffers returned by Read/Event are borrowed until the next mutation of core.
 */
typedef struct {
  uint32_t operation;
  uint32_t kind;
  uint64_t session;
  uint64_t revision;
  uint64_t ack;
  uint32_t start;
  uint32_t end;
  uint32_t caret;
  const uint16_t *text;
  uintptr_t length;
  const ShellPageClause *clauses;
  uintptr_t clause_count;
} ShellPageInputCommand;

typedef struct {
  uint64_t session;
  uint64_t revision;
  uint64_t edit;
  uint32_t start;
  uint32_t end;
  uint32_t length;
} ShellPageInputState;

typedef struct {
  uint64_t session;
  uint64_t edit;
  uint64_t revision;
  uint64_t request;
  uint32_t kind;
  uint32_t start;
  uint32_t end;
  uint32_t caret;
  const uint16_t *text;
  uintptr_t length;
  const ShellPageClause *clauses;
  uintptr_t clause_count;
} ShellPageInputEvent;

typedef struct {
  uint64_t session;
  uint64_t revision;
  uint64_t edit;
  uint64_t request;
  uint32_t start;
  uint32_t end;
  uint32_t valid;
  uint32_t clipped;
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
} ShellPageGeometry;

/*
 The window-request payload, byte for byte: 32 bytes, no padding.
 */
typedef struct {
  uint32_t kind;
  uint32_t flags;
  int32_t rect[4];
  int64_t value;
} ShellFrameWindowRequest;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
 A link end owned by the calling thread. Returns null for generation 0.
 # Safety
 `limits` must be null or readable.
 */
ShellLinkEnd *shell_link_create(uint32_t generation, const ShellLinkLimits *limits_in);

/*
 # Safety
 `link` must be null or from `shell_link_create`, and not used afterwards.
 */
void shell_link_destroy(ShellLinkEnd *link);

/*
 Queues one frame. Never blocks; `outcome` says what happened to it.
 # Safety
 `payload` must be readable for `length` bytes (or null with 0); `outcome` writable.
 */
ShellStatus shell_link_send(ShellLinkEnd *link,
                            uint32_t window,
                            uint16_t channel,
                            uint16_t kind,
                            uint64_t revision,
                            const uint8_t *payload,
                            uintptr_t length,
                            ShellLinkSend *outcome);

/*
 The next frame that may go to the transport, or null when none may. With
 `force`, credit is ignored: for a caller that will wait for the other side
 and must not overtake queued frames. The buffer is released with
 `shell_link_free_frame`, on any thread.
 # Safety
 `data` and `length` must be writable.
 */
ShellStatus shell_link_take(ShellLinkEnd *link, bool force, uint8_t **data, uintptr_t *length);

/*
 # Safety
 `data` and `length` must come from one `shell_link_take`, released once.
 */
void shell_link_free_frame(uint8_t *data, uintptr_t length);

/*
 Validates and accounts one whole frame. `Rejected`: from another
 generation, dropped. `Closed`: lost or forged frames closed the link.
 `InvalidArgument`: not a frame.
 # Safety
 `data` readable for `length` bytes and kept alive while `view` is used.
 */
ShellStatus shell_link_receive(ShellLinkEnd *link,
                               const uint8_t *data,
                               uintptr_t length,
                               ShellFrameView *view);

/*
 # Safety
 See `guard`.
 */
ShellStatus shell_link_retire(ShellLinkEnd *link, uint32_t window);

/*
 # Safety
 See `guard`.
 */
ShellStatus shell_link_close(ShellLinkEnd *link);

/*
 The serial for a local handle; with `create`, a new one if it has none.
 `UnknownTab` when it has none and `create` is false.
 # Safety
 `serial` must be writable.
 */
ShellStatus shell_link_serial(ShellLinkEnd *link, uintptr_t handle, bool create, uint32_t *serial);

/*
 Binds a serial the other side chose to a local handle.
 # Safety
 See `guard`.
 */
ShellStatus shell_link_adopt(ShellLinkEnd *link, uint32_t serial, uintptr_t handle);

/*
 # Safety
 `handle` must be writable.
 */
ShellStatus shell_link_handle(ShellLinkEnd *link, uint32_t serial, uintptr_t *handle);

/*
 # Safety
 `counts` must be writable.
 */
ShellStatus shell_link_counts(ShellLinkEnd *link, ShellLinkCounts *counts);

/*
 Encodes complete tab rows into a buffer released with `shell_link_free_frame`.
 # Safety
 `rows` readable for `count` entries and each title for its length; outputs writable.
 */
ShellStatus shell_frame_tabs_encode(const ShellFrameTabIn *rows,
                                    uintptr_t count,
                                    uint32_t selected,
                                    uint32_t revision,
                                    uint8_t **data,
                                    uintptr_t *length);

/*
 Decodes complete tab rows. Titles point into `payload`.
 # Safety
 `payload` readable for `length` bytes; `rows` writable for `capacity`; scalars writable.
 */
ShellStatus shell_frame_tabs_decode(const uint8_t *payload,
                                    uintptr_t length,
                                    ShellFrameTab *rows,
                                    uintptr_t capacity,
                                    uintptr_t *count,
                                    uint32_t *selected,
                                    uint32_t *revision);

/*
 Validate an ephemeral Share request against the current address/document identity.
 Borrowed payload; no state, allocation ownership or replay across connections.
 */
bool shell_frame_share_current(const uint8_t *payload,
                               uintptr_t length,
                               uint64_t session,
                               uint64_t document,
                               uint64_t now);

ShellPipeQueue *shell_pipe_queue_create(void);

void shell_pipe_queue_destroy(ShellPipeQueue *queue);

bool shell_pipe_queue_push(ShellPipeQueue *queue,
                           uint32_t route,
                           const uint8_t *data,
                           uintptr_t length);

bool shell_pipe_queue_pop(ShellPipeQueue *queue,
                          uint32_t *route,
                          uint8_t **data,
                          uintptr_t *length);

bool shell_pipe_header(uint32_t route, uintptr_t length, uint8_t *out);

/*
 Take only the calling owner's route. Other routes remain queued for their own threads.
 */
bool shell_pipe_queue_pop_route(ShellPipeQueue *queue,
                                uint32_t route,
                                uint8_t **data,
                                uintptr_t *length);

bool shell_pipe_decode_header(const uint8_t *data, uint32_t *route, uintptr_t *length);

bool shell_pipe_binding(ShellPipeBinding *binding,
                        uint32_t operation,
                        uint32_t serial,
                        uint64_t shell);

bool shell_recovery_history_merge(ShellRecoveryHistory a,
                                  ShellRecoveryHistory b,
                                  uint64_t now,
                                  ShellRecoveryHistory *out);

bool shell_recovery_export(const ShellRecovery *value, uint64_t now, ShellRecoveryLedger *out);

bool shell_recovery_import(ShellRecovery *value, ShellRecoveryLedger ledger, uint64_t now);

/*
 A read-only monotonic-time status query. Zero episode means no manual retry surface.
 */
ShellRecoveryRetry shell_recovery_retry_status(const ShellRecovery *value, uint64_t now);

/*
 Atomically reserve one explicit attempt without resetting the rolling ledger.
 */
bool shell_recovery_retry(ShellRecovery *value, uint64_t episode, uint64_t now);

ShellRecovery *shell_recovery_create(void);

void shell_recovery_destroy(ShellRecovery *value);

/*
 Operations: 0 begin, 1 finish, 2 permanent stop, 3 await peer exit (at most 2s), 4 query exhausted idle budget, 5 confirmed peer exit, 6 terminal awaiting user retry.
 */
bool shell_recovery_event(ShellRecovery *value,
                          uint32_t operation,
                          uint64_t now);

ShellRecoveryReadiness shell_recovery_readiness_begin(uint32_t generation,
                                                      uint32_t window,
                                                      uint32_t required,
                                                      uint64_t now);

/*
 Returns phase, or 3 for a stale identity, clock regression or unknown evidence bit.
 */
uint32_t shell_recovery_readiness_observe(ShellRecoveryReadiness *state,
                                          uint32_t generation,
                                          uint32_t window,
                                          uint32_t evidence,
                                          uint64_t now);

/*
 Pause only after the supplied resource prerequisites; resuming preserves remaining active time.
 */
uint32_t shell_recovery_readiness_pause(ShellRecoveryReadiness *state,
                                        uint32_t generation,
                                        uint32_t window,
                                        uint32_t prerequisites,
                                        uint64_t now);

uint32_t shell_recovery_readiness_resume(ShellRecoveryReadiness *state,
                                         uint32_t generation,
                                         uint32_t window,
                                         uint64_t now);

/*
 Kinds: 1 bind, 2 empty tab, 3 close/discard, 4 suppress restored, 5 new shell.
 # Safety
 Arrays are valid for their lengths, output has capacity slots, count is
 writable; outputs are disjoint from inputs. Failure leaves outputs untouched.
 */
bool shell_recovery_windows(uint64_t attempt,
                            const ShellRecoveryWindow *survivors,
                            uintptr_t ns,
                            const ShellRecoveryWindow *restored,
                            uintptr_t nr,
                            ShellRecoveryWindowEffect *output,
                            uintptr_t capacity,
                            uintptr_t *count);

/*
 # Safety
 Input is readable for length units; output is writable and disjoint.
 */
bool shell_reconnect_parse(const uint16_t *input,
                           uintptr_t length,
                           ShellReconnectDescriptor *output);

/*
 # Safety
 Output is writable and disjoint from core.
 */
ShellStatus shell_core_endpoint(ShellCore *core, uint64_t *output);

/*
 Revision zero inspects the current identity; otherwise accept a complete
 Firefox publication for the current endpoint. Key is empty or 32 lowercase
 hex UTF-16 units. Private publications must have an empty key.
 # Safety
 Input key is readable for length units; output is writable and disjoint.
 */
ShellStatus shell_core_recovery_identity(ShellCore *core,
                                         uint64_t session,
                                         uint64_t revision,
                                         uint32_t privacy,
                                         const uint16_t *key,
                                         uintptr_t length,
                                         ShellRecoveryWindowIdentity *output);

/*
 Accept a complete startup presentation for this native endpoint. Labels stay
 in the native presenter; shared Snapshot semantics own replay/stale/conflict.
 # Safety
 Fingerprint contains length UTF-16 units; apply is writable and disjoint.
 */
ShellStatus shell_core_startup_presentation(ShellCore *core,
                                            uint64_t session,
                                            uint64_t revision,
                                            const uint16_t *fingerprint,
                                            uintptr_t length,
                                            bool *apply);

/*
 Publish visible browser identities (revision > 0), or reproject current local
 state (revision == 0, empty input). Snapshot replay rules are shared. Draft
 editor state and acknowledged browser mappings are never mutated here.
 # Safety
 IDs reference length elements; output holds SHELL_STARTUP_ROW_CAPACITY rows.
 Count and output must be writable, mutually disjoint and disjoint from core.
 */
ShellStatus shell_core_startup_rows(ShellCore *core,
                                    uint64_t session,
                                    uint64_t generation,
                                    uint64_t revision,
                                    const uint64_t *ids,
                                    uintptr_t length,
                                    uint64_t selected,
                                    ShellStartupRow *output,
                                    uintptr_t capacity,
                                    uint32_t *count);

/*
 Operations: 0 inspect, 1 prepare context, 2 bind context/generation,
 3 create draft, 4 lease oldest submission, 5 close orphan draft,
 6 terminal completion (request/browser/accepted), 7 disconnect generation,
 8 lease next command (or inspect the outstanding lease), 9 claim wake,
 10 failed notification (permit a later retry). Wake operations are native-only.
 11 release editor to the selected published browser after terminal cleanup.
 12 select a restored editor, 13 return to a mapped local draft (native-only).
 14 adopt the selected published browser in place of an untouched placeholder.
 15 enable normal adoption, 16 commit a matching selected editor publication.
 Bind and later operations require the session returned by Prepare; later
 operations also require the exact bound generation. Context is an opaque
 host-established profile/privacy token, not an identifier inferred by Rust.
 Failed calls do not alter output. One outstanding command, no automatic retry.
 # Safety
 Output must be writable and disjoint from core-owned storage.
 */
ShellStatus shell_core_startup_transfer(ShellCore *core,
                                        uint32_t operation,
                                        uint64_t context,
                                        uint64_t session,
                                        uint64_t generation,
                                        uint64_t request,
                                        uint64_t draft,
                                        uint64_t browser,
                                        bool accepted,
                                        ShellStartupTransfer *output);

/*
 Read an acknowledged mapping by bounded index. This does not change any
 native editor or strip identity; the presenter must reconcile them explicitly.
 # Safety
 Output must be writable. Returned text is always null.
 */
ShellStatus shell_core_startup_mapping(ShellCore *core,
                                       uint64_t session,
                                       uint64_t generation,
                                       uintptr_t index,
                                       ShellStartupTransfer *output);

/*
 Atomic native editor observation; selection uses UTF-16 units.
 # Safety
 Text must reference length units. No borrowed buffer is retained.
 */
ShellStatus shell_core_startup_editor(ShellCore *core,
                                      uint64_t tab,
                                      const uint16_t *text,
                                      uintptr_t length,
                                      uint32_t start,
                                      uint32_t selected_length,
                                      bool composing);

/*
 Local-only operations: 0 inspect, 1 add, 2 edit, 3 select, 4 close,
 5 submit (argument=modifier bits shift/control/alt), 6 reorder (argument=index).
 No operation drains or acknowledges a browser command.
 # Safety
 Text must reference length UTF-16 units; output must be writable and disjoint.
 */
ShellStatus shell_core_startup(ShellCore *core,
                               uint32_t operation,
                               uint64_t tab,
                               uint32_t argument,
                               const uint16_t *text,
                               uintptr_t length,
                               ShellStartupView *output);

/*
 Read a draft (submission=false) or immutable pending submit by index.
 # Safety
 Output must be writable; returned text is borrowed until next core mutation.
 */
ShellStatus shell_core_startup_read(ShellCore *core,
                                    bool submission,
                                    uintptr_t index,
                                    ShellStartupView *output);

/*
 PrepareStartup uses shell only; Bind uses shell/content only. A prepared
 startup can bind only its original shell, with the endpoint generation intact.
 CancelStartup and other completion operations require the exact
 generation and request/handle returned by the binding. Inspect/RequestClose
 take a zeroed argument. Failed calls leave the output untouched.
 # Safety
 See [`guard`]. Input/output must point to valid, non-overlapping values.
 */
ShellStatus shell_core_window_host(ShellCore *core,
                                   uint32_t operation,
                                   const ShellWindowBinding *input,
                                   ShellWindowBinding *output);

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
 What one channel did with the publications it received, for diagnostics.
 Counts publications that reached the core; a malformed call rejected at
 the boundary is reported by its own status and not counted here.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_publication_stats(ShellCore *core,
                                         uint32_t channel,
                                         ShellPublicationCounts *out);

/*
 Validate a strip publication's envelope before the host stages anything.
 `unchanged` reports an exact replay of the accepted revision, which needs
 no work; otherwise `out` holds the options this publication resolves to.
 Mutates nothing.

 # Safety
 See [`guard`]. `envelope` must be readable; `fingerprint` readable for
 `units` values (null only for zero); `out` and `unchanged` writable.
 */
ShellStatus shell_core_strip_resolve(ShellCore *core,
                                     const ShellStripEnvelope *envelope,
                                     const uint16_t *fingerprint,
                                     uintptr_t units,
                                     ShellStripOptions *out,
                                     bool *unchanged);

/*
 Atomically accept a strip publication: envelope, removals, rows and the
 optional ordered group layout. Base zero replaces the retained state; other
 bases must match the accepted revision exactly. An exact replay is not a new
 publication and is refused here; resolve it first.

 On success `rows` receives one resolved row per input row, in order, saying
 which retained or published values each field takes, and `removed` receives
 the validated removal IDs. Nothing is written on failure.

 # Safety
 As [`shell_core_strip_resolve`]; `publication` and its arrays readable for
 their stated counts; `rows` and `removed` writable for `row_count` and
 `removal_count` entries (null only for zero); `out` writable.
 */
ShellStatus shell_core_strip_publish(ShellCore *core,
                                     const ShellStripEnvelope *envelope,
                                     const uint16_t *fingerprint,
                                     uintptr_t units,
                                     const ShellStripPublication *publication,
                                     ShellStripOptions *out,
                                     ShellStripRow *rows,
                                     uint32_t *removed);

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
                                       uint32_t revision,
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
                                          uint32_t revision,
                                          uint32_t tab,
                                          bool *changed);

/*
 Start a snapshot for `session`. Snapshots are built entry by entry so no
 array of pointers crosses the boundary. A second `begin` discards an
 unfinished one.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_model_begin_snapshot(ShellCore *core,
                                            uint64_t session,
                                            uint32_t revision,
                                            uint32_t capacity);

/*
 # Safety
 See [`guard`]. `title` must be valid for `title_len` `u16` values.
 */
ShellStatus shell_core_model_push_snapshot_tab(ShellCore *core,
                                               uint32_t tab,
                                               const uint16_t *title,
                                               uintptr_t title_len,
                                               uint32_t commands);

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
 # Safety
 See [`guard`]. Rows and their UTF-16 spans must be valid for the call.
 */
ShellStatus shell_core_model_apply_delta(ShellCore *core,
                                         uint32_t base,
                                         uint32_t revision,
                                         const ShellTabTitle *rows,
                                         uintptr_t row_count,
                                         bool has_selected,
                                         uint32_t selected);

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
 Observe a dwell candidate, or pass target=0 to cancel. Returns armed state.
 */
ShellStatus shell_core_group_hover(ShellCore *core,
                                   uint32_t source,
                                   uint32_t target,
                                   uint64_t now_ms,
                                   uint32_t delay_ms,
                                   bool *armed);

/*
 Apply this refresh's group surface motion to measured `surfaces` in place.
 `layout` is the accepted strip layout, read to tell when a held join has
 been published.

 # Safety
 See [`guard`]. `frame` and `out` are valid; `layout` and `surfaces` point
 to `layout_len` and `surfaces_len` elements (either may be null when empty).
 */
ShellStatus shell_core_group_surface_frame(ShellCore *core,
                                           const ShellGroupSurfaceFrame *frame,
                                           const ShellStripSlot *layout,
                                           uintptr_t layout_len,
                                           ShellGroupSurface *surfaces,
                                           uintptr_t surfaces_len,
                                           ShellGroupSurfaceResult *out);

/*
 A drop asked Firefox to join `tab` to `group`: hold the group's last
 dragged surface until the join is published or `until_ms` passes.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_group_surface_join(ShellCore *core,
                                          uint32_t group,
                                          uint32_t tab,
                                          double until_ms);

/*
 The next glow refresh shows its groups already lit (a full snapshot).

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_glow_seed(ShellCore *core);

/*
 Whether `group`'s glow is lit as of the last refresh.

 # Safety
 See [`guard`]. `lit` must be writable.
 */
ShellStatus shell_core_glow_is_lit(ShellCore *core, uint32_t group, bool *lit);

/*
 Advance glow lighting by one refresh over every glow-eligible group.
 `count` receives the number of changes; when it exceeds `capacity` the
 call returns `TooLarge` and commits nothing, so the host retries with a
 larger buffer. `disabled` reports light-up motion turning off.

 # Safety
 See [`guard`]. `groups` points to `len` elements and `output` to
 `capacity` (either may be null when zero); `count` and `disabled` are writable.
 */
ShellStatus shell_core_glow_advance(ShellCore *core,
                                    bool animate,
                                    bool halo,
                                    const ShellGlowGroup *groups,
                                    uintptr_t len,
                                    ShellGlowChange *output,
                                    uintptr_t capacity,
                                    uintptr_t *count,
                                    bool *disabled);

/*
 Record shown groups' settled extents and report those that grew; while
 `moving`, only forget groups no longer lit. At most `len` results, so a
 `len`-element `output` always suffices.

 # Safety
 See [`guard`]. `extents` points to `len` elements and `output` to as many
 (either may be null when zero); `count` is writable.
 */
ShellStatus shell_core_glow_settle(ShellCore *core,
                                   bool moving,
                                   const ShellGlowExtent *extents,
                                   uintptr_t len,
                                   ShellGlowRecatch *output,
                                   uintptr_t *count);

/*
 Record a tab row (`SHELL_SETTLE_TAB`) or group header
 (`SHELL_SETTLE_HEADER`) held at composition offset `dx` and left-anchored
 scale `sx`, easing to rest from `start_ms`.

 # Safety
 See [`guard`].
 */
ShellStatus shell_core_settle_hold(ShellCore *core,
                                   uint32_t kind,
                                   uint32_t id,
                                   float dx,
                                   float sx,
                                   double start_ms);

/*
 Where a held element is drawn at `now_ms`, relative to its layout bounds.

 # Safety
 See [`guard`]. `out` must be writable.
 */
ShellStatus shell_core_settle_sample(ShellCore *core,
                                     uint32_t kind,
                                     uint32_t id,
                                     double now_ms,
                                     ShellSettleSample *out);

/*
 Forget finished holds and report whether anything is still settling.
 With `clear`, forget every hold (motion was cancelled).

 # Safety
 See [`guard`]. `active` must be writable.
 */
ShellStatus shell_core_settle_advance(ShellCore *core, double now_ms, bool clear, bool *active);

/*
 Classify the validated address section (not the surrounding container).
 `sequence` orders publications; `revision` is query structure. Both retain
 JavaScript safe-integer width. Resolve with `commit=false` before mutation;
 commit only once application can proceed. Pending edits must not consume it.
 Session replacement resets identity; callers publish synchronously in order.

 # Safety
 See [`guard`]. Session and fingerprint must be readable for their lengths
 (null only for zero); `apply` must be writable.
 */
ShellStatus shell_core_address_publication(ShellCore *core,
                                           const uint16_t *session,
                                           uintptr_t session_units,
                                           uint64_t sequence,
                                           uint64_t revision,
                                           const uint16_t *fingerprint,
                                           uintptr_t units,
                                           bool commit,
                                           bool *apply);

/*
 Resolve/commit a bounded customization description, excluding independent
 labels. Unchanged permits label refresh; ignored preserves the live section.
 # Safety
 Live core, readable fingerprint (null only for zero), writable decision.
 */
ShellStatus shell_core_toolbar_publication(ShellCore *core,
                                           uint64_t revision,
                                           const uint16_t *fingerprint,
                                           uintptr_t units,
                                           bool commit,
                                           ShellToolbarPublication *decision);

/*
 Reset customization identity on removal, disable or island recreation.
 # Safety
 Live core.
 */
ShellStatus shell_core_toolbar_publication_reset(ShellCore *core);

/*
 Reset address publication identity on disable or island recreation.
 Request coordination has a separate lifetime and reset entry point.
 # Safety
 See [`guard`].
 */
ShellStatus shell_core_address_publication_reset(ShellCore *core);

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
 Test the promotion threshold.
 # Safety
 Live core, writable out.
 */
ShellStatus shell_core_tab_drag_ready(ShellCore *core, uint32_t tab, double x, double y, bool *out);

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
 Apply a compact selection context against existing publication membership.
 # Safety
 Live core; bounded readable selection and fingerprint; writable changed.
 */
ShellStatus shell_core_menu_selection(ShellCore *core,
                                      uint32_t base,
                                      uint32_t revision,
                                      const uint32_t *selected,
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
 Classify whether the group has been pulled off its strip, after
 `shell_core_group_drag_update` for the same view-local point. `width` and
 `height` are the strip view's size. `foreign` means another window's strip
 claims the pointer, which also takes the group off this strip.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_drag_detach(ShellCore *core,
                                         double x,
                                         double y,
                                         double width,
                                         double height,
                                         bool foreign);

/*
 Read the group, if any, leaving this strip for another window.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_group_departure_state(ShellCore *core, ShellGroupDeparture *out);

/*
 Keep `group` hidden while it leaves. `request` is this strip's own queued
 detach request, or 0 when another window's strip queued the adopt.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_departure_begin(ShellCore *core, uint32_t group, uint64_t request);

/*
 Another window's strip reports that Firefox refused to adopt `group`.
 # Safety
 Live core; changed is writable.
 */
ShellStatus shell_core_group_departure_reject(ShellCore *core, uint32_t group, bool *changed);

/*
 After a layout publication: whether the departing group has left it.
 # Safety
 Live core; left is writable.
 */
ShellStatus shell_core_group_departure_publish(ShellCore *core, bool *left);

/*
 Forget the departure once its visuals are restored or removed.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_departure_end(ShellCore *core);

/*
 Read this strip's state as the destination of another window's group drag.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_group_foreign_state(ShellCore *core, ShellForeignGroup *out);

/*
 Target another window's group between this strip's measured whole blocks
 (view-local logical axis, browser order, unshifted).
 # Safety
 Live core; slots reads len aligned entries, at most 10000 (may be null when 0).
 */
ShellStatus shell_core_group_foreign_hover(ShellCore *core,
                                           double x,
                                           const ShellGroupDragSlot *slots,
                                           uintptr_t len);

/*
 Visual offset opening the destination gap for one measured block.
 # Safety
 Live core; out is writable for one double.
 */
ShellStatus shell_core_group_foreign_offset(ShellCore *core,
                                            uintptr_t index,
                                            double width,
                                            double *out);

/*
 Hold the destination gap for a queued, tab-less adopt request.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_foreign_hold(ShellCore *core, uint64_t request);

/*
 End the destination preview. Without `force`, a held gap survives until its
 request is rejected; `force` is for a new authoritative layout.
 # Safety
 Live core; ended is writable.
 */
ShellStatus shell_core_group_foreign_end(ShellCore *core, bool force, bool *ended);

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

/*
 Pure bounded toolbar layout; no retained pointers or allocations. On rejection
 the output is untouched. Callers supply at most 55 readable slots.
 */
ShellStatus shell_core_toolbar_overflow(const ShellToolbarSlot *slots,
                                        uintptr_t len,
                                        double width,
                                        bool required_menu,
                                        uint64_t *out_mask);

/*
 Bounded pure hit testing for native customization. Output is unchanged on failure.
 */
ShellStatus shell_core_toolbar_drop_target(const ShellToolbarDropSlot *slots,
                                           uintptr_t len,
                                           double x,
                                           bool rtl,
                                           uint32_t *out_index);

/*
 Pack the bounded palette without changing persistent toolbar placement.
 # Safety
 Readable spans (null only for zero count), writable out.
 */
ShellStatus shell_core_toolbar_palette_order(const uint32_t *spans,
                                             uintptr_t count,
                                             uint32_t columns,
                                             ShellToolbarPaletteOrder *out);

/*
 Plan a bounded preview permutation without changing persistent toolbar state.
 */
ShellStatus shell_core_toolbar_preview(uintptr_t len,
                                       uintptr_t source,
                                       uintptr_t target,
                                       ShellToolbarPreview *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_surface_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_surface_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_surface_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_surface_menu_resolve(ShellCore *core,
                                            uint32_t session,
                                            uint32_t revision,
                                            const uint32_t *actions,
                                            uintptr_t count,
                                            const uint16_t *fingerprint,
                                            uintptr_t units,
                                            bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_surface_menu_accept(ShellCore *core,
                                           uint32_t session,
                                           uint32_t revision,
                                           const uint32_t *actions,
                                           uintptr_t count,
                                           const uint16_t *fingerprint,
                                           uintptr_t units,
                                           bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_surface_menu_activate(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             uint32_t action,
                                             bool *accepted);

/*
 Open a preview request; IDs stay safe JavaScript integers and never wrap.
 # Safety
 See [`guard`]. `request` must be writable.
 */
ShellStatus shell_core_preview_begin(ShellCore *core, uint32_t tab, uint64_t *request);

/*
 Dismiss only the named opening; an old close cannot revoke a newer request.
 # Safety
 See [`guard`].
 */
ShellStatus shell_core_preview_dismiss(ShellCore *core, uint64_t request);

/*
 Resolve/commit fully validated preview content before native mutation.
 Phase 1 has no wire image key; phase 2 has an image key, including null.
 # Safety
 See [`guard`]. The fingerprint span must be readable and `apply` writable.
 */
ShellStatus shell_core_preview_publication(ShellCore *core,
                                           uint32_t tab,
                                           uint64_t request,
                                           uint32_t phase,
                                           const uint16_t *fingerprint,
                                           uintptr_t fingerprint_len,
                                           bool commit,
                                           bool *apply);

/*
 Classify/commit a C++-validated address section (channels 7..=9).
 Modes fingerprints exclude image/applied updates; menus include the full body.
 Native opening/focus/edit/target preconditions are supplied as eligibility.
 # Safety
 See [`guard`]. All input spans are readable; decision is writable.
 */
ShellStatus shell_core_address_section(ShellCore *core,
                                       uint32_t channel,
                                       const uint16_t *session,
                                       uintptr_t session_len,
                                       uint64_t generation,
                                       const uint16_t *fingerprint,
                                       uintptr_t fingerprint_len,
                                       bool eligible,
                                       bool commit,
                                       ShellAddressSectionDecision *decision);

/*
 Test publication eligibility without consuming its generation or content.
 # Safety
 See [`guard`]. Context and its spans are readable; eligible is writable.
 */
ShellStatus shell_core_address_menu_eligible(ShellCore *core,
                                             const ShellAddressMenuContext *context,
                                             bool *eligible);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_downloads_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_downloads_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_downloads_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_downloads_panel_resolve(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               const uint32_t *actions,
                                               uintptr_t count,
                                               const uint16_t *fingerprint,
                                               uintptr_t units,
                                               bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_downloads_panel_accept(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_downloads_panel_activate(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                uint32_t action,
                                                bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_profile_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_profile_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_profile_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_profile_menu_resolve(ShellCore *core,
                                            uint32_t session,
                                            uint32_t revision,
                                            const uint32_t *actions,
                                            uintptr_t count,
                                            const uint16_t *fingerprint,
                                            uintptr_t units,
                                            bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_profile_menu_accept(ShellCore *core,
                                           uint32_t session,
                                           uint32_t revision,
                                           const uint32_t *actions,
                                           uintptr_t count,
                                           const uint16_t *fingerprint,
                                           uintptr_t units,
                                           bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_profile_menu_activate(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             uint32_t action,
                                             bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_overflow_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_overflow_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_overflow_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_overflow_menu_resolve(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_overflow_menu_accept(ShellCore *core,
                                            uint32_t session,
                                            uint32_t revision,
                                            const uint32_t *actions,
                                            uintptr_t count,
                                            const uint16_t *fingerprint,
                                            uintptr_t units,
                                            bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_overflow_menu_activate(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              uint32_t action,
                                              bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_developer_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_developer_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_developer_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_developer_menu_resolve(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_developer_menu_accept(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_developer_menu_activate(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               uint32_t action,
                                               bool *accepted);

/*
 Allocate an independent privacy-panel opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_privacy_panel_open(ShellCore *core, uint32_t kind, uint32_t *out);

/*
 Revoke one privacy panel without resetting its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_privacy_panel_dismiss(ShellCore *core, uint32_t kind);

/*
 Resolve a bounded confirmation description before native mutation.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_privacy_panel_resolve(ShellCore *core,
                                             uint32_t kind,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Accept a bounded confirmation description using Session publication semantics.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_privacy_panel_accept(ShellCore *core,
                                            uint32_t kind,
                                            uint32_t session,
                                            uint32_t revision,
                                            const uint32_t *actions,
                                            uintptr_t count,
                                            const uint16_t *fingerprint,
                                            uintptr_t units,
                                            bool *changed);

/*
 Consume one current privacy confirmation action once.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_privacy_panel_activate(ShellCore *core,
                                              uint32_t kind,
                                              uint32_t session,
                                              uint32_t revision,
                                              uint32_t action,
                                              bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_tab_groups_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_tab_groups_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_tab_groups_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_tab_groups_menu_resolve(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               const uint32_t *actions,
                                               uintptr_t count,
                                               const uint16_t *fingerprint,
                                               uintptr_t units,
                                               bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_tab_groups_menu_accept(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_tab_groups_menu_activate(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                uint32_t action,
                                                bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_group_header_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_group_header_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_group_header_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_group_header_resolve(ShellCore *core,
                                            uint32_t session,
                                            uint32_t revision,
                                            const uint32_t *actions,
                                            uintptr_t count,
                                            const uint16_t *fingerprint,
                                            uintptr_t units,
                                            bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_group_header_accept(ShellCore *core,
                                           uint32_t session,
                                           uint32_t revision,
                                           const uint32_t *actions,
                                           uintptr_t count,
                                           const uint16_t *fingerprint,
                                           uintptr_t units,
                                           bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_group_header_activate(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             uint32_t action,
                                             bool retained,
                                             bool *accepted);

/*
 Resolve a browser-initiated group creation opening once, through Snapshot rules.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_group_header_request(ShellCore *core,
                                            uint32_t request,
                                            uint32_t group,
                                            uint32_t window,
                                            uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_profiler_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_profiler_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_profiler_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_profiler_panel_resolve(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_profiler_panel_accept(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_profiler_panel_activate(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               uint32_t action,
                                               bool retained,
                                               bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_bookmark_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_bookmark_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_bookmark_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_bookmark_panel_resolve(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_bookmark_panel_accept(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_bookmark_panel_activate(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               uint32_t action,
                                               bool retained,
                                               bool *accepted);

/*
 Resolve a browser-owned bookmark editor opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_bookmark_panel_request(ShellCore *core,
                                              uint32_t request,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_permissions_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_permissions_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_permissions_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_permissions_panel_resolve(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 const uint32_t *actions,
                                                 uintptr_t count,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_permissions_panel_accept(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                const uint32_t *actions,
                                                uintptr_t count,
                                                const uint16_t *fingerprint,
                                                uintptr_t units,
                                                bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_permissions_panel_activate(ShellCore *core,
                                                  uint32_t session,
                                                  uint32_t revision,
                                                  uint32_t action,
                                                  bool retained,
                                                  bool *accepted);

/*
 Resolve a browser-owned site permissions opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_permissions_panel_request(ShellCore *core,
                                                 uint32_t request,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_connection_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_connection_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_connection_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_connection_panel_resolve(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                const uint32_t *actions,
                                                uintptr_t count,
                                                const uint16_t *fingerprint,
                                                uintptr_t units,
                                                bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_connection_panel_accept(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               const uint32_t *actions,
                                               uintptr_t count,
                                               const uint16_t *fingerprint,
                                               uintptr_t units,
                                               bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_connection_panel_activate(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 uint32_t action,
                                                 bool retained,
                                                 bool *accepted);

/*
 Resolve a browser-owned connection security opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_connection_panel_request(ShellCore *core,
                                                uint32_t request,
                                                const uint16_t *fingerprint,
                                                uintptr_t units,
                                                uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_translations_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_translations_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_translations_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_translations_panel_resolve(ShellCore *core,
                                                  uint32_t session,
                                                  uint32_t revision,
                                                  const uint32_t *actions,
                                                  uintptr_t count,
                                                  const uint16_t *fingerprint,
                                                  uintptr_t units,
                                                  bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_translations_panel_accept(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 const uint32_t *actions,
                                                 uintptr_t count,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_translations_panel_activate(ShellCore *core,
                                                   uint32_t session,
                                                   uint32_t revision,
                                                   uint32_t action,
                                                   bool retained,
                                                   bool *accepted);

/*
 Resolve a browser-owned translations opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_translations_panel_request(ShellCore *core,
                                                  uint32_t request,
                                                  const uint16_t *fingerprint,
                                                  uintptr_t units,
                                                  uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_selected_translations_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_selected_translations_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_selected_translations_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_selected_translations_panel_resolve(ShellCore *core,
                                                           uint32_t session,
                                                           uint32_t revision,
                                                           const uint32_t *actions,
                                                           uintptr_t count,
                                                           const uint16_t *fingerprint,
                                                           uintptr_t units,
                                                           bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_selected_translations_panel_accept(ShellCore *core,
                                                          uint32_t session,
                                                          uint32_t revision,
                                                          const uint32_t *actions,
                                                          uintptr_t count,
                                                          const uint16_t *fingerprint,
                                                          uintptr_t units,
                                                          bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_selected_translations_panel_activate(ShellCore *core,
                                                            uint32_t session,
                                                            uint32_t revision,
                                                            uint32_t action,
                                                            bool retained,
                                                            bool *accepted);

/*
 Resolve a browser-owned translations opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_selected_translations_panel_request(ShellCore *core,
                                                           uint32_t request,
                                                           const uint16_t *fingerprint,
                                                           uintptr_t units,
                                                           uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_page_context_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_page_context_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_page_context_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_page_context_menu_resolve(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 const uint32_t *actions,
                                                 uintptr_t count,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_page_context_menu_accept(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                const uint32_t *actions,
                                                uintptr_t count,
                                                const uint16_t *fingerprint,
                                                uintptr_t units,
                                                bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_page_context_menu_activate(ShellCore *core,
                                                  uint32_t session,
                                                  uint32_t revision,
                                                  uint32_t action,
                                                  bool retained,
                                                  bool *accepted);

/*
 Resolve a browser-owned page context opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_page_context_menu_request(ShellCore *core,
                                                 uint32_t request,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 uint32_t *out);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_back_forward_menu_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_back_forward_menu_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_back_forward_menu_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_back_forward_menu_resolve(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 const uint32_t *actions,
                                                 uintptr_t count,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_back_forward_menu_accept(ShellCore *core,
                                                uint32_t session,
                                                uint32_t revision,
                                                const uint32_t *actions,
                                                uintptr_t count,
                                                const uint16_t *fingerprint,
                                                uintptr_t units,
                                                bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_back_forward_menu_activate(ShellCore *core,
                                                  uint32_t session,
                                                  uint32_t revision,
                                                  uint32_t action,
                                                  bool *accepted);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_all_tabs_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_all_tabs_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_all_tabs_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_all_tabs_panel_resolve(ShellCore *core,
                                              uint32_t session,
                                              uint32_t revision,
                                              const uint32_t *actions,
                                              uintptr_t count,
                                              const uint16_t *fingerprint,
                                              uintptr_t units,
                                              bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_all_tabs_panel_accept(ShellCore *core,
                                             uint32_t session,
                                             uint32_t revision,
                                             const uint32_t *actions,
                                             uintptr_t count,
                                             const uint16_t *fingerprint,
                                             uintptr_t units,
                                             bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_all_tabs_panel_activate(ShellCore *core,
                                               uint32_t session,
                                               uint32_t revision,
                                               uint32_t action,
                                               bool retained,
                                               bool *accepted);

/*
 Project a bounded realized list onto a stable opaque row ID.
 # Safety
 Live core, readable rows (null only for zero count), writable outputs.
 */
ShellStatus shell_core_all_tabs_panel_project(ShellCore *core,
                                              const ShellAllTabsRow *rows,
                                              uintptr_t count,
                                              double y,
                                              double top,
                                              double bottom,
                                              uint32_t *out,
                                              bool *before);

/*
 Center a promoted secondary-list drag in the strip's measured row geometry.
 # Safety
 Live core on its owner thread.
 */
ShellStatus shell_core_tab_drag_list_origin(ShellCore *core);

/*
 Allocate a window-scoped menu opening. Zero means identity exhaustion.
 # Safety
 Live core and writable out.
 */
ShellStatus shell_core_notification_panel_open(ShellCore *core, uint32_t *out);

/*
 Revoke a window-scoped menu, preserving its identity high-water mark.
 # Safety
 Live core.
 */
ShellStatus shell_core_notification_panel_dismiss(ShellCore *core);

/*
 Resolve a validated session description before mutating the host container.
 Accepted work is committed and counted by shell_core_notification_panel_accept;
 ignored/refused work ends here, without changing menu or action identity.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_notification_panel_resolve(ShellCore *core,
                                                  uint32_t session,
                                                  uint32_t revision,
                                                  const uint32_t *actions,
                                                  uintptr_t count,
                                                  const uint16_t *fingerprint,
                                                  uintptr_t units,
                                                  bool *changed);

/*
 Accept a complete bounded description atomically; unchanged descriptions do not rebuild controls.
 # Safety
 Live core, readable arrays (null only for zero length), writable changed.
 */
ShellStatus shell_core_notification_panel_accept(ShellCore *core,
                                                 uint32_t session,
                                                 uint32_t revision,
                                                 const uint32_t *actions,
                                                 uintptr_t count,
                                                 const uint16_t *fingerprint,
                                                 uintptr_t units,
                                                 bool *changed);

/*
 Consume one current enabled action; dismissal and late updates cannot resurrect it.
 # Safety
 Live core and writable accepted.
 */
ShellStatus shell_core_notification_panel_activate(ShellCore *core,
                                                   uint32_t session,
                                                   uint32_t revision,
                                                   uint32_t action,
                                                   bool retained,
                                                   bool *accepted);

/*
 Resolve a browser-owned notification opening once through Snapshot rules.
 # Safety
 Live core, readable fingerprint and writable out.
 */
ShellStatus shell_core_notification_panel_request(ShellCore *core,
                                                  uint32_t request,
                                                  const uint16_t *fingerprint,
                                                  uintptr_t units,
                                                  uint32_t *out);

/*
 Native gesture/focus observations. kind is ShellAddressInteractionKind;
 value is a boolean. Tokens are endpoint-local and survive session resets.
 # Safety
 core is live; out is writable. Invalid observations never mutate state.
 */
ShellStatus shell_core_address_interaction(ShellCore *core,
                                           uint32_t kind,
                                           uint32_t pointer,
                                           uint64_t token,
                                           uint32_t value,
                                           ShellAddressInteractionState *out);

/*
 Allocate native chrome and content in physical client pixels, without
 retaining state. Invalid inputs leave output untouched; fits=false is a
 valid result requiring host fallback, not a partially applicable layout.
 # Safety
 Non-null pointers must be aligned, readable/writable and not overlap.
 */
ShellStatus shell_core_chrome_layout(const ShellChromeLayoutInput *input,
                                     ShellChromeLayout *output);

/*
 Test a bounded geometry observation for one ShellLayoutWork consumer.
 Explicit force is for active motion or a host-owned invalidation. The host
 invalidates again on failed work and on topology/realization changes.
 # Safety
 See [`guard`]. `inputs` and `changed` point to valid values.
 */
ShellStatus shell_core_layout_observe(ShellCore *core,
                                      uint32_t consumer,
                                      const ShellLayoutInputs *inputs,
                                      bool force,
                                      bool *changed);

/*
 Invalidate one consumer without changing any other consumer's observation.
 # Safety
 See [`guard`].
 */
ShellStatus shell_core_layout_invalidate(ShellCore *core, uint32_t consumer);

/*
 # Safety
 Valid non-overlapping pointers; text readable for length units during call.
 */
ShellStatus shell_core_page_input_apply(ShellCore *core, const ShellPageInputCommand *input);

/*
 # Safety
 core is live and exclusively accessed by its native owner. Ok when a geometry request is
 still waiting to be read, Busy otherwise.
 */
ShellStatus shell_core_page_geometry_pending(ShellCore *core);

/*
 # Safety
 Valid output pointers. Returned text is borrowed until next core mutation.
 */
ShellStatus shell_core_page_input_read(ShellCore *core,
                                       ShellPageInputState *state,
                                       const uint16_t **text);

/*
 # Safety
 Valid output pointer. Busy means empty; returned text is borrowed until mutation.
 */
ShellStatus shell_core_page_input_event(ShellCore *core, ShellPageInputEvent *out);

/*
 # Safety
 core is live and exclusively accessed by its native owner.
 */
ShellStatus shell_core_page_geometry_context(ShellCore *core, uint64_t session, uint64_t revision);

/*
 # Safety
 out is writable; core is live and exclusively accessed.
 */
ShellStatus shell_core_page_geometry_read(ShellCore *core,
                                          uint32_t start,
                                          uint32_t end,
                                          ShellPageGeometry *out);

/*
 # Safety
 input is readable; core is live and exclusively accessed.
 */
ShellStatus shell_core_page_geometry_reply(ShellCore *core, const ShellPageGeometry *input);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* FENN_SHELL_CORE_H */
