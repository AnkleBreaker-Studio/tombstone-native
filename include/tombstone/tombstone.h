#ifndef TOMBSTONE_TOMBSTONE_H
#define TOMBSTONE_TOMBSTONE_H

/*
 * Tombstone Native SDK — crash & telemetry client for any game engine.
 *
 * Pure C99 ABI: no C++ types cross this boundary and no call ever throws.
 * Every entry point returns a tombstone_result. The SDK is fail-silent by
 * design: internal failures degrade to result codes and (optionally) lines on
 * the user-provided log callback — never to console output or exceptions.
 *
 * The SDK binds to one implicit process-wide client (tombstone_handle is the
 * opaque type reserved for a future multi-instance API). Double-init and
 * use-after-shutdown return result codes; they are never undefined behavior.
 *
 * Crash capture scope: this SDK REPORTS crashes you hand it
 * (tombstone_report_crash) and detects unclean shutdowns across launches. As of
 * v0.9 it can ALSO install an in-process async-signal-safe native crash handler
 * (opt-in via options.enable_native_crash_handler, default off; Linux/Android) —
 * see that field. Full stack unwinding (Breakpad) + Windows SEH + iOS Mach
 * remain on the roadmap (see README).
 *
 * Pre-init capture (v0.7): tombstone_add_breadcrumb / tombstone_track_event /
 * tombstone_track_metric / tombstone_set_user / tombstone_set_environment /
 * tombstone_set_user_metadata / tombstone_set_consent /
 * tombstone_start_session called BEFORE tombstone_init BUFFER (bounded:
 * breadcrumbs 64 drop-oldest; events+metrics 128 combined drop-newest;
 * last-write-wins for identity/environment/metadata/consent) and are replayed
 * in order at init, AFTER the options are applied — an explicit pre-init Set*
 * beats the matching init option.
 */

#include <stddef.h>

#if defined(_WIN32)
#if defined(TOMBSTONE_STATIC)
#define TOMBSTONE_API
#elif defined(TOMBSTONE_EXPORTS)
#define TOMBSTONE_API __declspec(dllexport)
#else
#define TOMBSTONE_API __declspec(dllimport)
#endif
#else /* !_WIN32 */
#if defined(TOMBSTONE_EXPORTS)
#define TOMBSTONE_API __attribute__((visibility("default")))
#else
#define TOMBSTONE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque client handle (reserved; the v0 API drives the process-wide instance). */
typedef struct tombstone_handle tombstone_handle;

/** Result of every API call. Negative values are errors; >= 0 is non-fatal. */
typedef enum tombstone_result {
    TOMBSTONE_OK = 0,
    /** Call ignored: consent is off or the feature is disabled. Not an error. */
    TOMBSTONE_DISABLED = 1,
    /** Suppressed as a duplicate (crash dedupe window). Not an error. */
    TOMBSTONE_SUPPRESSED = 2,
    TOMBSTONE_ERROR_INVALID_ARGUMENT = -1,
    TOMBSTONE_ERROR_NOT_INITIALIZED = -2,
    TOMBSTONE_ERROR_ALREADY_INITIALIZED = -3,
    TOMBSTONE_ERROR_IO = -4,
    TOMBSTONE_ERROR_QUEUE_FULL = -5,
    TOMBSTONE_ERROR_TIMEOUT = -6,
    TOMBSTONE_ERROR_INTERNAL = -7
} tombstone_result;

/** Severity for breadcrumbs and session-log lines. */
typedef enum tombstone_level {
    TOMBSTONE_LEVEL_DEBUG = 0,
    TOMBSTONE_LEVEL_INFO = 1,
    TOMBSTONE_LEVEL_WARN = 2,
    TOMBSTONE_LEVEL_ERROR = 3
} tombstone_level;

/**
 * Optional sink for the SDK's own diagnostics (the SDK never writes to
 * stdout/stderr). `level` is a tombstone_level value. Must be thread-safe;
 * may be invoked from SDK background threads. Must not call back into the SDK.
 */
typedef void (*tombstone_log_callback)(int level, const char *message, void *user_data);

/**
 * Initialization options. Zero it, call tombstone_options_init() to apply
 * defaults, then override fields. All strings are copied during init; the
 * caller keeps ownership of the memory it passed.
 */
typedef struct tombstone_options {
    /** Tombstone base URL, e.g. "https://your-tenant.example.com". Required. */
    const char *endpoint;
    /** Per-game SDK token ("tmb_..."). Treat as a build secret. Required. */
    const char *token;
    /** Shipped with every payload (dashboard build filter). Required. */
    const char *build_version;
    /** Writable directory for sidecars, session log, marker. NULL = platform default. */
    const char *data_dir;
    /** Deployment environment stamped on every payload (e.g. "production",
     *  "staging", "dev"; clamped to 64). NULL/empty = unset (the server
     *  defaults to "production"). tombstone_set_environment() overrides this. */
    const char *environment;
    /** Seconds between session heartbeats; clamped to [15, 240]. 0 = default (60).
     *  The ceiling is a BILLING constant, not a preference: the server merges
     *  consecutive beats into one live session only while the gap stays under its
     *  300s window, so a slower cadence shatters a continuous session into isolated
     *  instants and the studio's monthly peak-CCU invoice comes out low. */
    int heartbeat_interval_s;
    /** Nonzero (default) = emit periodic session heartbeats. */
    int enable_heartbeats;
    /** Nonzero (default) = keep a rolling session log, uploadable with reports. */
    int enable_session_log;
    /** Nonzero (default) = detect a hard-killed previous session and report it. */
    int enable_unclean_shutdown_detection;
    /** Nonzero (default) = capture starts immediately. 0 = wait for tombstone_set_consent(1). */
    int consent_granted;
    /** Nonzero (default) = the SDK begins SENDING immediately at init (today's
     *  behavior). 0 = defer the first heartbeat and all event/metric batch
     *  uploads until tombstone_start_session(): without the gate the first
     *  beat leaves as a provisional-identity "production" session before the
     *  game has set identity / environment / metadata (v0.8: the device-derived
     *  `dev_...` id, never anonymous). Crash and bug reports are never
     *  deferred. */
    int auto_start_session;
    /** Nonzero (default) = emit a `tombstone.rtt_ms` metric after each successful ingest
     *  POST (the upload round-trip time). 0 disables this self-telemetry. */
    int enable_rtt_metric;
    /** EXPERIMENTAL (v0.9), default 0 (OFF): install the in-process native crash
     *  handler. When on, the SDK catches fatal signals (SIGSEGV/ABRT/BUS/ILL/
     *  FPE/TRAP) with an async-signal-safe handler that writes a minimal dump
     *  ((module, build-id, offset) frames — NO on-device symbolication) and
     *  chains to any pre-existing handler (Unity / UGS); the dump is turned
     *  into a crash report at the NEXT launch. POSIX/ELF only (Linux/Android)
     *  — a no-op elsewhere. Off keeps the existing unclean-shutdown heuristic.
     *  Enable only with the game engine's own crash handler installed FIRST so
     *  chaining is exercised. */
    int enable_native_crash_handler;
    /** Optional diagnostics sink (see tombstone_log_callback). NULL = silent. */
    tombstone_log_callback log_callback;
    /** Opaque pointer handed back to log_callback. */
    void *log_callback_user_data;
} tombstone_options;

/** Fill `options` with documented defaults. Safe on an uninitialized struct. */
TOMBSTONE_API tombstone_result tombstone_options_init(tombstone_options *options);

/**
 * Initialize the SDK: resolves the data dir, rotates the previous session log,
 * reads the unclean-shutdown marker, drains offline sidecars, and starts the
 * background upload + heartbeat threads. First successful call wins;
 * subsequent calls return TOMBSTONE_ERROR_ALREADY_INITIALIZED.
 */
TOMBSTONE_API tombstone_result tombstone_init(const tombstone_options *options);

/**
 * Clean shutdown: flushes the session log, persists undelivered durable
 * payloads as sidecars, deletes the unclean-shutdown marker, and joins the
 * background threads. Idempotent.
 */
TOMBSTONE_API tombstone_result tombstone_shutdown(void);

/**
 * Attribute subsequent payloads to a player. Values are clamped to the wire
 * contract (user_id 128, steam_id 32 chars).
 *
 * Device identity (v0.8): the SDK never sends an anonymous user. At init it
 * acquires a persistent device-derived provisional id ("dev_" + 16 lowercase
 * hex — the machine id hashed salted with the game token; the raw machine id
 * never goes on the wire), and every payload carries it until you set a real
 * id. The first tombstone_set_user(real_id) after init upgrades the SAME
 * session: the provisional id rides heartbeats (and crash/bug bodies) once as
 * `priorUserId` until a heartbeat is acked, so the server merges the session's
 * pre-login telemetry under the real player. Passing NULL/empty (logout)
 * reverts to the provisional id and drops the pending marker — logged-out
 * telemetry never merges into anyone.
 *
 * Changing to a DIFFERENT user id also drops the per-user metadata map (see
 * tombstone_set_user_metadata) so one player's attributes never bleed onto
 * the next; a provisional/anonymous -> id login keeps it.
 */
TOMBSTONE_API tombstone_result tombstone_set_user(const char *user_id, const char *steam_id);

/**
 * Replace the per-user metadata map (a displayName + any studio-defined
 * string attributes), carried as the `metadata` object on session heartbeats
 * with change detection: a beat carries the map only when it differs from the
 * last map a heartbeat delivered, and clearing sends `{}` once so the server
 * deletes the stored record. REPLACE semantics: every call swaps the whole
 * map; pass NULL/0 to clear it. `keys`/`values` are parallel arrays of length
 * `count`, clamped to the wire contract (at most 16 pairs, keys <= 64 bytes,
 * values <= 512 bytes, UTF-8-safe); NULL/empty keys or values are skipped (an
 * empty value means "unset" server-side). The reserved key "displayName"
 * becomes the player's primary label in the dashboard. The map is keyed to
 * the current user: tombstone_set_user to a different id drops it.
 */
TOMBSTONE_API tombstone_result tombstone_set_user_metadata(const char *const *keys,
                                                           const char *const *values,
                                                           size_t count);

/**
 * Toggle capture + upload (store policy / GDPR). While 0, nothing is recorded
 * or sent — breadcrumbs, heartbeats, events, and reports are all ignored.
 */
TOMBSTONE_API tombstone_result tombstone_set_consent(int granted);

/**
 * Release the send gate: begin heartbeats and event/metric batch uploads. A
 * one-way idempotent latch — the first call wins, repeats are no-ops (both
 * return TOMBSTONE_OK). Meaningful with tombstone_options.auto_start_session
 * set to 0, which holds the gate so the game can call tombstone_set_user /
 * tombstone_set_environment / tombstone_set_user_metadata FIRST: without it
 * the first heartbeat leaves as a provisional-identity "production" session
 * before the game has configured who is playing (v0.8: the device-derived
 * `dev_...` id — never anonymous). While the gate is held the heartbeat loop
 * sends NOTHING and event/metric batch drains are held (items still buffer,
 * bounded, and ship once started); crash and bug reports still send — their
 * write-ahead path is never deferred. Calling this BEFORE tombstone_init is
 * remembered: the latch survives into init regardless of auto_start_session.
 */
TOMBSTONE_API tombstone_result tombstone_start_session(void);

/**
 * Set the deployment environment (e.g. "production", "staging", "dev"),
 * stamped as `environment` on every subsequent payload — crashes, bug reports,
 * events, metrics, and heartbeats (clamped to 64 chars; the server defaults to
 * "production" when absent). Overrides tombstone_options.environment. A
 * NULL/empty value is rejected with TOMBSTONE_ERROR_INVALID_ARGUMENT and the
 * current environment is retained — the environment can never be blanked.
 */
TOMBSTONE_API tombstone_result tombstone_set_environment(const char *environment);

/**
 * Set server-lifetime fleet labels, emitted on HEARTBEATS only (matching the
 * Unity SDK): `region` (clamped to 64) and `hostname` (clamped to 255) let the
 * dashboard group and locate dedicated servers. Pass NULL to keep a label
 * unchanged; pass "" to clear it (a cleared/empty label is omitted from the
 * wire body).
 */
TOMBSTONE_API tombstone_result tombstone_set_server_info(const char *region,
                                                         const char *hostname);

/**
 * Mark this process as a DEDICATED SERVER without requiring a match: flips
 * role to "server" for all subsequent telemetry and sets `server_id` (clamped
 * to 128). A NULL/empty `server_id` keeps the existing id — the server
 * identity is never blanked. `region`/`hostname` are optional fleet labels
 * (see tombstone_set_server_info); NULL/empty keeps the current value.
 *
 * Game CLIENTS must never call this: a client that claims role="server"
 * pollutes the Fleet registry and can be targeted by server-scoped log pulls.
 * Call it once at boot on dedicated-server builds only.
 */
TOMBSTONE_API tombstone_result tombstone_mark_dedicated_server(const char *server_id,
                                                               const char *region,
                                                               const char *hostname);

/**
 * Tag subsequent telemetry with multiplayer correlation context. `server_id`
 * and `match_id` are stamped (clamped to 128 chars) onto every crash, bug, and
 * event body so server<->match<->session linking is exact. Pass NULL to clear
 * either. Empty/cleared dimensions are omitted from the wire body.
 *
 * `h` is the reserved opaque handle (the v0 API drives the process-wide
 * instance); pass NULL. Fail-soft: a no-op before init.
 */
TOMBSTONE_API void tombstone_set_match_context(tombstone_handle *h, const char *server_id,
                                               const char *match_id);

/**
 * Begin a server-authoritative match: marks role="server", mints a fresh match
 * id, and stamps it on subsequent telemetry until tombstone_end_match(). The
 * 32-char id (plus NUL) is written to `out_match_id`; `out_cap` must be >= 33.
 * Returns TOMBSTONE_ERROR_INVALID_ARGUMENT for a NULL/too-small buffer (the
 * cached context is left unchanged) and TOMBSTONE_ERROR_NOT_INITIALIZED before
 * init. `h` is the reserved opaque handle; pass NULL.
 */
TOMBSTONE_API tombstone_result tombstone_start_match(tombstone_handle *h, char *out_match_id,
                                                     size_t out_cap);

/**
 * End the current match: clears the cached match id so subsequent telemetry is
 * no longer match-tagged (role and server id are retained). `h` is the reserved
 * opaque handle; pass NULL. Fail-soft: a no-op before init.
 */
TOMBSTONE_API void tombstone_end_match(tombstone_handle *h);

/**
 * Device specs, engine-agnostic by design: the SDK reports what you hand it —
 * it does NOT probe hardware (your engine already knows its GPU/CPU/screen).
 * Zero the struct; every field is optional and NULL/0 means "unset" (unset
 * fields are omitted from the wire body). Strings are clamped to the wire
 * contract maxima noted per field.
 */
typedef struct tombstone_device_t {
    const char *model;             /* <=256 device/hardware model name */
    const char *type;              /* <=32  e.g. "Desktop", "Handheld", "Console" */
    const char *os;                /* <=256 full OS string with version */
    const char *os_family;         /* <=48  e.g. "Windows", "Linux", "macOS" */
    const char *cpu;               /* <=256 CPU model string */
    int cpu_count;                 /* logical core count; 0 = unset */
    int ram_mb;                    /* system memory in MB; 0 = unset */
    const char *gpu;               /* <=256 GPU device name */
    const char *gpu_vendor;        /* <=256 GPU vendor name */
    const char *gpu_version;       /* <=256 driver/API version string */
    const char *gpu_api;           /* <=64  e.g. "Direct3D12", "Vulkan", "Metal" */
    int vram_mb;                   /* video memory in MB; 0 = unset */
    const char *screen;            /* <=32  "WxH", e.g. "2560x1440" */
    double screen_dpi;             /* dots per inch; 0 = unset */
    double refresh_rate;           /* display Hz; 0 = unset */
    const char *orientation;       /* <=32  e.g. "Landscape", "Portrait" */
    int fullscreen;                /* nonzero = fullscreen; 0 = unset/omitted */
    const char *language;          /* <=48  system/user language tag */
    const char *engine;            /* <=48  e.g. "Unreal 5.4", "Godot 4.3" */
    const char *scripting_backend; /* <=32  e.g. "Native", "Mono", "GDScript" */
    const char *platform;          /* <=48  store/runtime platform label */
} tombstone_device_t;

/**
 * Cache device specs for subsequent payloads. The struct is deep-copied (the
 * caller keeps ownership of every string). Emitted as the `device` JSON object
 * on crash and bug-report bodies, and on heartbeats UNTIL ONE HEARTBEAT IS
 * ACKED by the server (then dropped from beats for the rest of the session; a
 * lost beat re-carries it). Pass NULL to clear the cached device. Set it once
 * after init — device specs rarely change mid-session.
 */
TOMBSTONE_API tombstone_result tombstone_set_device(const tombstone_device_t *device);

/**
 * Add a breadcrumb to the trail attached to future crash and bug reports.
 * Fixed 64-slot ring; oldest entries are overwritten. Message clamped to 512.
 */
TOMBSTONE_API tombstone_result tombstone_add_breadcrumb(tombstone_level level, const char *message);

/**
 * Track a named analytics event with optional flat string attributes
 * (parallel keys[]/values[] arrays of length `count`; both may be NULL when
 * count is 0). Clamped to the wire contract: 32 attributes, 64-char keys,
 * 512-char values, 64-char name. Retried with backoff; persisted offline on
 * final failure.
 */
TOMBSTONE_API tombstone_result tombstone_track_event(const char *name,
                                                     const char *const *keys,
                                                     const char *const *values,
                                                     size_t count);

/**
 * Record a numeric metric sample (e.g. tick rate, RTT, memory use). Batched
 * client-side and flushed on count/age/quit/pre-crash like analytics events;
 * each sample carries its own occurredAtIso plus the cached correlation
 * context (role/serverId/matchId/sessionId). `value` must be finite (NaN /
 * Infinity is dropped). `unit` is an optional short label (e.g. "ms", "fps",
 * "hz"); NULL or "" omits it. `h` is the reserved opaque handle; pass NULL.
 * Fail-soft: a no-op before init or while consent is off.
 */
TOMBSTONE_API void tombstone_track_metric(tombstone_handle *h, const char *name, double value,
                                          const char *unit);

/**
 * Feed one rendered frame's duration into the frame-stats accumulator — call
 * once per frame from the render/game loop.
 *
 * UNITS: `frame_ms` is MILLISECONDS — 16.7 for 60 FPS, 33.3 for 30 FPS.
 * Passing SECONDS (e.g. a raw 0.0166 delta-time) is NOT detectable and
 * produces plausible-looking garbage: every frame appears ~1000x faster, so
 * fpsAvg pins at the 1000 clamp and slow frames / hitches silently vanish.
 * Double-check the conversion at the call site (`delta_seconds * 1000.0`).
 *
 * NOT async-signal-safe: the accumulator takes an internal spinlock. Never
 * call this from a signal handler, SEH / vectored exception filter, or any
 * other crash-time context — a handler interrupting the lock holder on the
 * same thread would spin forever (deadlock).
 *
 * Allocation-free and lock-light on the normal path, built to be called
 * every frame from any regular thread. Each heartbeat drains the accumulated
 * window onto the beat as `fpsAvg`, `slowFramePct` (frames > 33.4 ms — a
 * missed 30 FPS budget), `hitchCount` (frames > 250 ms — a visible hitch on
 * any target) and `worstFrameMs`, then resets for the next interval (all
 * values clamped to the wire contract). Non-finite and <= 0 samples are
 * ignored. Fail-soft: a no-op before init or while consent is off. Dedicated
 * servers with no rendering never call it.
 */
TOMBSTONE_API void tombstone_report_frame(double frame_ms);

/**
 * Report a crash. `signature` groups occurrences (clamped to 128 chars); pass
 * NULL to derive a stable signature from stack_hint + stack_trace. stack_hint
 * is the one-line summary (clamped to 512; NULL becomes "Crash"); stack_trace
 * is optional (clamped to 8192). attach_log != 0 requests a session-log
 * upload slot. Write-ahead durable: persisted to disk before the first upload
 * attempt. Deduped per signature (<= 1 report/min; repeats return
 * TOMBSTONE_SUPPRESSED and become a counter breadcrumb).
 */
TOMBSTONE_API tombstone_result tombstone_report_crash(const char *signature,
                                                      const char *stack_hint,
                                                      const char *stack_trace,
                                                      int attach_log);

/**
 * Submit a player bug report. `message` is required (clamped to 4000);
 * `category` is optional (clamped to 32). The current breadcrumb trail is
 * attached. attach_log != 0 requests a session-log upload slot. Write-ahead
 * durable like crashes.
 */
TOMBSTONE_API tombstone_result tombstone_report_bug(const char *category,
                                                    const char *message,
                                                    int attach_log);

/**
 * Mirror one game-log line into the rolling session log (512 KB cap with
 * front-trim) and the breadcrumb ring. This is the integration point for an
 * engine's log hook.
 */
TOMBSTONE_API tombstone_result tombstone_log_line(tombstone_level level, const char *line);

/**
 * Server-side: queue a log pull for a player / session / match / whole server.
 * `target_type` is one of "userId" | "sessionId" | "matchId" | "server"
 * (clamped to 32 chars); `target_value` is the corresponding id (clamped to
 * 128); `reason` is the audit note (clamped to 280). Requires a WRITE-scoped
 * server token — an ingest-only client token is rejected server-side (the 403
 * is logged via the diagnostics callback, never fatal). The pull is queued; the
 * targeted client(s) upload on their next heartbeat (consent-gated). Fail-silent:
 * a no-op before init or on a NULL/empty argument. `h` is the reserved opaque
 * handle; pass NULL.
 */
TOMBSTONE_API void tombstone_request_player_logs(tombstone_handle *h, const char *target_type,
                                                 const char *target_value, const char *reason);

/**
 * Server-side convenience: auto-pull a player's logs after an anomalous
 * disconnect. Equivalent to tombstone_request_player_logs(h, "userId", user_id,
 * reason); a NULL/empty `reason` defaults to "anomalous disconnect". `h` is the
 * reserved opaque handle; pass NULL. Fail-silent.
 */
TOMBSTONE_API void tombstone_on_anomalous_disconnect(tombstone_handle *h, const char *user_id,
                                                     const char *reason);

/**
 * Block until the outbound queue is drained (delivered, persisted offline, or
 * dropped as poison) or `timeout_ms` elapses. Returns TOMBSTONE_ERROR_TIMEOUT
 * when work remains. timeout_ms <= 0 means "do not wait, just report".
 */
TOMBSTONE_API tombstone_result tombstone_flush(int timeout_ms);

/**
 * Set a per-name keep-rate (sampling) for events and metrics. `rate` is clamped to
 * [0,1]: 1 keeps everything (the default for any unset name), 0 drops everything, and
 * a value in between keeps that fraction at random. Below-rate samples are dropped
 * BEFORE buffering, so a chatty stream costs nothing once sampled out. The rate table
 * is bounded; `name` is required. `h` is the reserved opaque handle; pass NULL.
 * Fail-soft: a no-op before init or on a NULL/empty name.
 */
TOMBSTONE_API void tombstone_set_sample_rate(tombstone_handle *h, const char *name, double rate);

/**
 * Record the current level / scene name as context: adds an info breadcrumb
 * ("[scene] level: <name>") to the crash trail and stores `level_name` as the current
 * level context. `name` is clamped to the breadcrumb message limit. `h` is the reserved
 * opaque handle; pass NULL. Fail-soft: a no-op before init, while consent is off, or on
 * a NULL/empty name.
 */
TOMBSTONE_API void tombstone_set_level(tombstone_handle *h, const char *level_name);

/**
 * Point-in-time snapshot of SDK state for support overlays / dev HUDs (K3). Fill a
 * caller-owned struct; allocates nothing and never throws. `last_flush_age_seconds` is
 * -1.0 when no event/metric batch has flushed yet this launch. Returns
 * TOMBSTONE_ERROR_INVALID_ARGUMENT for a NULL `out` and TOMBSTONE_ERROR_NOT_INITIALIZED
 * before init (with `out` zeroed). `h` is the reserved opaque handle; pass NULL.
 */
typedef struct tombstone_diagnostics_t {
    /** Nonzero once tombstone_init has completed. */
    int initialized;
    /** Current telemetry consent state (nonzero = granted). */
    int consent_granted;
    /** Payloads waiting in the in-memory outbound queue. */
    int queued_outbound;
    /** Write-ahead payloads persisted to the offline sidecar directory. */
    int persisted_sidecar;
    /** Seconds since the last event/metric batch flush, or -1.0 when none yet this launch. */
    double last_flush_age_seconds;
    /** Nonzero when a match id is currently set. */
    int has_match_id;
    /** Nonzero when a server id is currently set. */
    int has_server_id;
} tombstone_diagnostics_t;

TOMBSTONE_API tombstone_result tombstone_diagnostics(tombstone_handle *h,
                                                     tombstone_diagnostics_t *out);

/** SDK semantic version string, e.g. "0.1.0". Never NULL. */
TOMBSTONE_API const char *tombstone_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TOMBSTONE_TOMBSTONE_H */
