#include "client.h"

#include "clock.h"
#include "device_identity.h"
#include "json_scan.h"
#include "json_writer.h"
#include "payloads.h"
#include "platform.h"
#include "signature.h"
#include "transport.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;

namespace tombstone {

namespace {

constexpr const char *crashes_path = "/api/v1/ingest/crashes";
constexpr const char *bug_reports_path = "/api/v1/ingest/bug-reports";
constexpr const char *events_path = "/api/v1/ingest/events";
constexpr const char *heartbeats_path = "/api/v1/ingest/heartbeats";
constexpr const char *events_batch_path = "/api/v1/ingest/events:batch";
constexpr const char *metrics_batch_path = "/api/v1/ingest/metrics:batch";
constexpr const char *pull_requests_path = "/api/v1/pull-requests";

constexpr int min_heartbeat_interval_s = 15;
// 240, NOT 600 — the server's session window is 300s and this is a BILLING constant.
//
// computePeakConcurrency merges consecutive beats into one live span only while the gap is <= that
// window. At 600s a player online for an hour produces six isolated instants instead of one span, so
// concurrent players stop overlapping, the monthly PEAK CCU comes out low, and the studio is invoiced
// on the low figure. Unity shipped exactly this (0.19.3, MAX_HEARTBEAT_INTERVAL_SECONDS = 600f) and
// the Tombstack repo carries tests/heartbeat-cadence.test.ts to stop it recurring — but that guard read
// the C# source only, which is why this side kept 600. It now reads the C header's range as well.
//
// The 20% headroom is deliberate: at exactly the window each beat expires as the next arrives, so any
// scheduling jitter or upload latency opens a gap.
constexpr int max_heartbeat_interval_s = 240;
constexpr int default_heartbeat_interval_s = 60;

constexpr const char *identity_file_name = "identity";

constexpr const char *unclean_signature = "unclean-shutdown";
constexpr const char *unclean_stack_hint =
    "Previous session ended without a clean shutdown (hard crash, OOM kill, or force quit)";

std::string random_session_id() {
    std::mt19937_64 engine{std::random_device{}()};
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int chunk = 0; chunk < 2; ++chunk) {
        const std::uint64_t value = engine();
        for (unsigned i = 0; i < 16; ++i) {
            out += digits[(value >> (i * 4U)) & 0xFU];
        }
    }
    return out;
}

const char *ingest_path_for(SidecarKind kind) noexcept {
    switch (kind) {
    case SidecarKind::crash:
        return crashes_path;
    case SidecarKind::bug_report:
        return bug_reports_path;
    case SidecarKind::event:
    default:
        return events_path;
    }
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

}  // namespace

Client::Client() = default;

Client::~Client() {
    try {
        stop_heartbeat();
        if (session_log_enabled_ && storage_available_) {
            session_log_.flush_now();
        }
        if (worker_) {
            // Quit flush (spec section 16): drain buffered events/metrics and
            // give the worker a bounded window to deliver before we tear down.
            flush_all_batches();
            worker_->flush(std::chrono::milliseconds{2000});
            worker_->stop();  // persists durable leftovers as sidecars
        }
        if (storage_available_) {
            marker_.remove();  // clean shutdown: next launch sees no dirty marker
        }
    } catch (...) {
        // Destructor must never throw.
    }
}

tombstone_result Client::init(const tombstone_options &options) {
    if (options.endpoint == nullptr || options.endpoint[0] == '\0' ||
        options.token == nullptr || options.token[0] == '\0' ||
        options.build_version == nullptr || options.build_version[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }

    sdk_log_.configure(options.log_callback, options.log_callback_user_data);

    endpoint_ = trim_trailing_slash(options.endpoint);
    token_ = options.token;
    build_version_ = options.build_version;
    os_ = os_name();
    arch_ = arch_name();
    session_id_ = random_session_id();
    // Deployment environment: the init option seeds it; an explicit set_environment()
    // call later overrides. Empty/absent -> unset (the server defaults to "production").
    if (options.environment != nullptr && options.environment[0] != '\0') {
        environment_ = std::string{utf8_safe_truncate(options.environment, limits::environment)};
    }
    heartbeats_enabled_ = options.enable_heartbeats != 0;
    session_log_enabled_ = options.enable_session_log != 0;
    unclean_detection_enabled_ = options.enable_unclean_shutdown_detection != 0;
    rtt_metric_enabled_ = options.enable_rtt_metric != 0;
    native_crash_enabled_ = options.enable_native_crash_handler != 0;
    consent_ = options.consent_granted != 0;
    // v0.7 send gate: nonzero (default) latches immediately — today's behavior.
    // 0 defers heartbeats + batch drains until start_session(); a pre-init
    // tombstone_start_session() is replayed by the C layer right after init
    // (StartGate::set survives this arm — see start_gate.h).
    start_gate_.arm(options.auto_start_session != 0);

    const int interval = options.heartbeat_interval_s > 0 ? options.heartbeat_interval_s
                                                          : default_heartbeat_interval_s;
    heartbeat_interval_ = std::chrono::seconds{
        std::clamp(interval, min_heartbeat_interval_s, max_heartbeat_interval_s)};

    data_dir_ = (options.data_dir != nullptr && options.data_dir[0] != '\0')
                    ? fs::path{options.data_dir}
                    : default_data_dir();
    configure_storage();
    // v0.9: install the native crash handler once storage is ready (dump fd +
    // module table). Opt-in + fail-soft: unsupported platforms / setup failure
    // just leave native_crash_enabled_ behavior to the unclean-shutdown
    // heuristic. Must run AFTER the previous dump was picked up (above).
    if (native_crash_enabled_ && storage_available_) {
        native_crash::install(data_dir_, sdk_log_);
    }
    // v0.8: acquire the device-derived provisional user id BEFORE anything can
    // send (worker/heartbeats start below), so no payload ever ships anonymous.
    acquire_device_identity();

    transport_ = std::make_unique<Transport>(sdk_log_);
    worker_ = std::make_unique<Worker>(*transport_, sidecars_, session_log_, sdk_log_, token_);
    worker_->set_batch_drainer([this] { drain_ready_batches(); });
    worker_->set_ack_handler([this](const std::string &body) { handle_heartbeat_ack(body); });
    if (rtt_metric_enabled_) {
        worker_->set_rtt_handler([this](double ms) { record_rtt_metric(ms); });
    }
    worker_->start();

    initialized_ = true;
    drain_sidecars();

    if (heartbeats_enabled_) {
        heartbeat_thread_ = std::thread{[this] { heartbeat_loop(); }};
    }
    if (consent_) {
        start_session_tracking();
    }
    return TOMBSTONE_OK;
}

/** Resolve the data dir and wire up the disk-backed subsystems. Degrades to
 * memory-only operation (no sidecars, no session log) when the dir is unusable. */
void Client::configure_storage() {
    try {
        std::error_code ec;
        fs::create_directories(data_dir_, ec);
        if (ec && !fs::exists(data_dir_, ec)) {
            sdk_log_.warn("data dir unavailable; running without offline persistence");
            return;
        }
        storage_available_ = true;
        sidecars_.configure(data_dir_);
        marker_.configure(data_dir_);
        if (session_log_enabled_) {
            session_log_.configure(data_dir_);
            had_previous_log_ = session_log_.rotate_for_new_session();
        }
        if (unclean_detection_enabled_) {
            previous_marker_ = marker_.take_previous();
        } else {
            marker_.remove();  // never leave a stale marker behind
        }
        // v0.9: pick up a native crash dump from the previous run BEFORE the
        // handler is (re)installed below — install() truncates the dump file.
        if (native_crash_enabled_) {
            previous_native_dump_ = native_crash::take_previous(data_dir_, sdk_log_);
        }
    } catch (const std::exception &e) {
        storage_available_ = false;
        sdk_log_.warn(std::string{"storage setup failed: "} + e.what());
    }
}

/**
 * v0.8 device identity (Unity 0.16 parity): restore the persisted provisional
 * user id, or derive + persist a fresh one, so the SDK NEVER sends an
 * anonymous user. The id is "dev_" + 16 lowercase hex of FNV-1a-64 over the
 * machine id salted with the per-game ingest token — the raw machine id never
 * goes on the wire, and the same machine yields a different id per game. The
 * FILE WINS over a re-derive on later launches, so the id stays stable across
 * an ingest-token rotation. Fail-soft throughout: worst case (no machine id
 * AND no writable storage) is a random-but-well-formed id per launch.
 */
void Client::acquire_device_identity() {
    std::string id = read_persisted_identity();
    if (id.empty()) {
        std::string source = read_machine_id();
        if (source.empty()) {
            // No stable machine id on this platform/config: a random source
            // still yields a well-formed id (persisted below when possible).
            sdk_log_.warn("machine id unavailable; deriving a random device identity");
            source = random_session_id();
        }
        id = device_identity::derive(token_, source);
        persist_identity(id);
    }
    const std::lock_guard<std::mutex> lock(user_mutex_);
    provisional_user_id_ = std::move(id);
}

std::string Client::read_persisted_identity() {
    if (!storage_available_) {
        return {};
    }
    try {
        const fs::path path = data_dir_ / identity_file_name;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return {};
        }
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            return {};
        }
        std::string id;
        std::getline(in, id);
        while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' ')) {
            id.pop_back();
        }
        // A corrupt/hand-edited file re-derives instead of putting junk on the wire.
        return device_identity::is_valid(id) ? id : std::string{};
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not read device identity: "} + e.what());
        return {};
    }
}

void Client::persist_identity(const std::string &id) {
    if (!storage_available_) {
        return;
    }
    try {
        std::ofstream out{data_dir_ / identity_file_name, std::ios::binary | std::ios::trunc};
        if (!out) {
            sdk_log_.warn("could not persist device identity (open failed)");
            return;
        }
        out << id;
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not persist device identity: "} + e.what());
    }
}

/** Re-queue payloads persisted by an earlier run. Restored records came from
 * a previous session: a granted log presign must upload that run's preserved
 * log, never this session's fresh one. */
void Client::drain_sidecars() {
    for (SidecarRecord &record : sidecars_.scan()) {
        if (record.kind == SidecarKind::crash) {
            has_restored_crash_ = true;
        }
        UploadJob job;
        job.url = endpoint_ + ingest_path_for(record.kind);
        job.body = std::move(record.body);
        job.durability = Durability::write_ahead;
        job.kind = record.kind;
        job.sidecar_path = record.path;
        job.request_log = (record.kind != SidecarKind::event) &&
                          find_bool_field(job.body, "log").value_or(false);
        job.log_from_previous = true;
        job.sign_body = true;  // restored crash/bug/event sidecars are ingest POSTs (S3)
        worker_->enqueue(std::move(job));
    }
}

/**
 * Start dirty-session tracking exactly once per launch, the first time capture
 * is allowed (at init, or at set_consent(true) when consent was deferred):
 * write this session's marker and report the previous unclean shutdown, if any.
 */
void Client::start_session_tracking() {
    {
        const std::lock_guard<std::mutex> lock(session_tracking_mutex_);
        if (session_tracking_started_ || !unclean_detection_enabled_ || !storage_available_) {
            return;
        }
        session_tracking_started_ = true;
    }
    marker_.write(SessionMarkerData{session_id_, now_iso8601_utc_ms(), build_version_, os_, arch_});
    if (previous_marker_.has_value()) {
        const SessionMarkerData previous = *previous_marker_;
        previous_marker_.reset();
        report_unclean_shutdown(previous);
    }
}

/**
 * The previous run left its marker behind -> it died hard. No double counting:
 * when a crash sidecar from that session was restored, the death is already
 * represented (and that report's log presign carries previous-session.log).
 * Only when no crash survived do we send this synthetic report.
 */
void Client::report_unclean_shutdown(const SessionMarkerData &previous) {
    if (has_restored_crash_) {
        // A managed crash sidecar survived — richer than anything we can
        // synthesize; consume any native dump so it can't double-report.
        previous_native_dump_.reset();
        return;
    }
    // v0.9: a captured native crash dump beats the generic heuristic — it
    // carries the real signal + symbolicatable frames. Report it INSTEAD of the
    // "unclean-shutdown" placeholder for the same dead session.
    if (previous_native_dump_.has_value()) {
        const NativeCrashDump dump = *previous_native_dump_;
        previous_native_dump_.reset();
        report_native_crash(previous, dump);
        return;
    }
    CrashPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();  // detection time
    payload.build_version = previous.build_version.empty() ? build_version_ : previous.build_version;
    payload.os = previous.os.empty() ? os_ : previous.os;
    payload.arch = previous.arch.empty() ? arch_ : previous.arch;
    payload.signature = unclean_signature;  // constant -> all unclean shutdowns group together
    payload.stack_hint = unclean_stack_hint;
    payload.user_id = current_user_id();
    payload.steam_id = current_steam_id();
    payload.environment = current_match_context().environment;
    payload.device = current_device();  // same physical machine, captured this launch
    // This launch's crumbs belong to this session, not the dead one.
    payload.log = session_log_enabled_ && had_previous_log_;
    enqueue_ingest(crashes_path, build_crash_json(payload), Durability::write_ahead,
                   SidecarKind::crash, payload.log, /*log_from_previous=*/true);
}

/**
 * v0.9: turn a native crash dump captured last run into a crash report. The
 * signature is derived from the top native frames (module+offset) so the same
 * native bug groups together; the human hint names the signal; the frames go
 * in stackTrace as "module+offset buildId" lines for server-side symbolication
 * against uploaded symbols. crashType/osSignal ride as fields (never in the
 * signature). A SIGABRT is classified "signal" (a managed abort/assert path)
 * vs. the memory-access signals as "native_crash".
 */
void Client::report_native_crash(const SessionMarkerData &previous, const NativeCrashDump &dump) {
    std::string frames_text;
    for (const NativeCrashFrame &f : dump.frames) {
        frames_text += f.module;
        frames_text += '+';
        char off[20];
        std::snprintf(off, sizeof(off), "0x%llx", static_cast<unsigned long long>(f.offset));
        frames_text += off;
        if (!f.build_id.empty()) {
            frames_text += ' ';
            frames_text += f.build_id;
        }
        frames_text += '\n';
    }

    const std::string sig_name = native_crash::signal_name(dump.signal_no);
    char fault[24];
    std::snprintf(fault, sizeof(fault), "0x%llx", static_cast<unsigned long long>(dump.fault_addr));

    CrashPayload payload;
    payload.occurred_at_iso = now_iso8601_utc_ms();  // detection time (dead session's ts is off-window)
    payload.build_version = previous.build_version.empty() ? build_version_ : previous.build_version;
    payload.os = previous.os.empty() ? os_ : previous.os;
    payload.arch = previous.arch.empty() ? arch_ : previous.arch;
    // Signature over the native frames (falls back to the signal when frames
    // were unresolved) — mirrors compute_crash_signature's hint+frames hashing.
    payload.signature = compute_crash_signature(sig_name, frames_text);
    payload.stack_hint = "Native crash: " + sig_name + " at " + fault;
    payload.stack_trace = frames_text;
    payload.user_id = current_user_id();
    payload.steam_id = current_steam_id();
    payload.environment = current_match_context().environment;
    payload.device = current_device();
    payload.log = session_log_enabled_ && had_previous_log_;
    // SIGABRT (POSIX signal 6) is the managed abort/assert/std::terminate path
    // -> "signal"; the memory-access faults are "native_crash". Literal 6 (not
    // the SIGABRT macro) so the classification tracks the DEVICE's signal number
    // regardless of the host the SDK was built on (Windows maps SIGABRT to 22).
    payload.crash_type = (dump.signal_no == 6) ? "signal" : "native_crash";
    payload.os_signal = dump.signal_no;
    enqueue_ingest(crashes_path, build_crash_json(payload), Durability::write_ahead,
                   SidecarKind::crash, payload.log, /*log_from_previous=*/true);
}

void Client::heartbeat_loop() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    while (!heartbeat_stop_) {
        // v0.7 send gate: no beat leaves before start_session() — otherwise the
        // first heartbeat registers a provisional-identity "production" session
        // before the game has set identity/environment/metadata (v0.8: the
        // device-derived dev_ id, never anonymous).
        if (capture_allowed() && start_gate_.is_set()) {
            try {
                HeartbeatPayload payload;
                payload.session_id = session_id_;
                payload.occurred_at_iso = now_iso8601_utc_ms();
                payload.build_version = build_version_;
                payload.os = os_;
                payload.arch = arch_;
                // Identity snapshot (one lock so id + prior can never diverge):
                // the effective user id (provisional fallback — never anonymous,
                // v0.8) plus the pending one-shot priorUserId merge marker. The
                // marker rides every beat until one carrying it is acked; the
                // value recorded in prior_in_flight_ commits on the next 2xx
                // (mirrors the device/metadata carry — a lost beat re-carries).
                {
                    const std::lock_guard<std::mutex> user_lock(user_mutex_);
                    payload.user_id = user_id_.empty() ? provisional_user_id_ : user_id_;
                    if (!pending_prior_user_id_.empty()) {
                        payload.prior_user_id = pending_prior_user_id_;
                        prior_in_flight_ = pending_prior_user_id_;
                    }
                }
                // role/serverId/matchId let the server register Fleet servers and
                // honor match/server log-pulls. role is always present: default to
                // "client" until a match marks this actor a "server".
                const MatchContext ctx = current_match_context();
                payload.role = ctx.role.empty() ? std::string{"client"} : ctx.role;
                payload.server_id = ctx.server_id;
                payload.match_id = ctx.match_id;
                payload.environment = ctx.environment;
                // Server-lifetime fleet labels (region/hostname) group dedicated servers.
                const ServerInfo info = current_server_info();
                payload.region = info.region;
                payload.hostname = info.hostname;
                // Device specs ride the beat until one is acked, then never again this
                // session (a lost beat re-carries — see device_sent_on_heartbeat_).
                bool carrying_device = false;
                if (!device_sent_on_heartbeat_.load(std::memory_order_relaxed)) {
                    DevicePayload device = current_device();
                    if (device_has_content(device)) {
                        payload.device = std::move(device);
                        carrying_device = true;
                    }
                }
                // Per-user metadata (change detection, Unity M1): carry the map
                // only when its canonical JSON differs from the last one a beat
                // ACKED — a steady map costs no repeat server writes, a clear
                // sends "{}" once. The in-flight json/epoch commit on the next
                // heartbeat ack; a set_user epoch bump voids a stale commit (M2).
                {
                    const std::lock_guard<std::mutex> md_lock(metadata_mutex_);
                    std::string current = build_user_metadata_json(user_metadata_);
                    if (current != metadata_last_acked_json_) {
                        metadata_in_flight_json_ = current;
                        metadata_in_flight_epoch_ = metadata_epoch_;
                        payload.metadata_json = std::move(current);
                    }
                }
                // Frame stats: drain this interval's window onto the beat and
                // reset (omitted entirely when no frame was sampled — dedicated
                // servers and menus without report_frame calls stay clean).
                const FrameStatsSnapshot frames = frame_stats_.consume();
                if (frames.valid) {
                    payload.has_frame_stats = true;
                    payload.fps_avg = frames.fps_avg;
                    payload.slow_frame_pct = frames.slow_frame_pct;
                    payload.hitch_count = frames.hitch_count;
                    payload.worst_frame_ms = frames.worst_frame_ms;
                }
                UploadJob job;
                job.url = endpoint_ + heartbeats_path;
                job.body = build_heartbeat_json(payload);
                job.durability = Durability::ephemeral;  // a missed beat is stale data
                job.parse_ack = true;  // the 202 ack may carry pull requests for us
                job.sign_body = true;  // heartbeats are an ingest POST — sign them (S3)
                if (carrying_device) {
                    device_carry_in_flight_.store(true, std::memory_order_relaxed);
                }
                worker_->enqueue(std::move(job));
            } catch (const std::exception &e) {
                sdk_log_.warn(std::string{"heartbeat build failed: "} + e.what());
            }
        }
        heartbeat_cv_.wait_for(lock, heartbeat_interval_,
                               [this] { return heartbeat_stop_ || heartbeat_kick_; });
        heartbeat_kick_ = false;  // consumed: the loop top sends the kicked beat now
    }
}

void Client::stop_heartbeat() {
    {
        const std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_stop_ = true;
    }
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

/**
 * Heartbeat command channel: for each pending pull request in the ack that
 * targets THIS client (by userId/sessionId/matchId/serverId) — and only while
 * consent is granted — POST a fulfilment that asks the server to presign a log
 * slot, with request_log set so the existing logUpload chase PUTs the current
 * session log off-thread (spec section 15). Runs on the worker thread; fail-soft
 * throughout — a non-targeted or non-consented client uploads nothing.
 */
void Client::handle_heartbeat_ack(const std::string &response_body) {
    try {
        // Fires on every heartbeat 2xx (worker.cpp). If the beat that just delivered was
        // carrying the device snapshot, mark it sent so subsequent beats drop it. Done
        // before the body/consent early-returns so an empty ack still confirms delivery.
        if (device_carry_in_flight_.exchange(false, std::memory_order_relaxed)) {
            device_sent_on_heartbeat_.store(true, std::memory_order_relaxed);
        }
        {
            // v0.8: the delivered beat carried prior_in_flight_ as its
            // priorUserId — stop sending the one-shot marker. Cleared only
            // when it still equals the pending value, so a late ack cannot
            // clobber a marker re-stamped by a newer set_user transition
            // (same commit discipline as the metadata epoch guard below).
            const std::lock_guard<std::mutex> user_lock(user_mutex_);
            if (!prior_in_flight_.empty() && prior_in_flight_ == pending_prior_user_id_) {
                pending_prior_user_id_.clear();
            }
            prior_in_flight_.clear();
        }
        {
            // Commit the metadata the delivered beat carried (M1). Voided when
            // the epoch moved: set_user reset the baseline after this beat was
            // built, so a pre-change beat must not clobber the new identity's
            // baseline (M2). Beats are serialized through the one worker queue,
            // so like the device flag above this tolerates a lost beat by simply
            // re-carrying on the next one.
            const std::lock_guard<std::mutex> md_lock(metadata_mutex_);
            if (metadata_in_flight_epoch_ == metadata_epoch_ &&
                !metadata_in_flight_json_.empty()) {
                metadata_last_acked_json_ = metadata_in_flight_json_;
            }
            metadata_in_flight_json_.clear();
            metadata_in_flight_epoch_ = -1;
        }
        if (!capture_allowed() || response_body.empty()) {
            return;
        }
        const std::vector<PendingPullRequest> pending = find_pending_requests(response_body);
        if (pending.empty()) {
            return;
        }
        const std::string user_id = current_user_id();
        const MatchContext ctx = current_match_context();
        for (const PendingPullRequest &request : pending) {
            if (request.request_id.empty()) {
                continue;
            }
            if (!pull_request_targets_client(request.target_type, request.target_value, user_id,
                                             session_id_, ctx.match_id, ctx.server_id)) {
                continue;
            }
            UploadJob job;
            job.url = endpoint_ + pull_requests_path + "/" + request.request_id + "/fulfill";
            job.body = build_pull_fulfill_json(user_id, session_id_, ctx.match_id, ctx.server_id,
                                               request.fulfill_nonce, request.nonce_expiry);
            job.durability = Durability::persist_on_failure;  // retried in-session with backoff
            job.request_log = true;  // chase data.logUpload -> PUT the current session log
            job.no_persist = true;   // the presign is time-sensitive; never sidecar across launches
            worker_->enqueue(std::move(job));
        }
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"heartbeat ack handling failed: "} + e.what());
    } catch (...) {
        sdk_log_.warn("heartbeat ack handling failed (unknown error)");
    }
}

tombstone_result Client::set_consent(bool granted) {
    const bool was_granted = consent_.exchange(granted);
    if (granted && initialized_) {
        start_session_tracking();
    } else if (!granted && was_granted) {
        // Consent revoked: purge buffered breadcrumbs so the pre-revoke trail can't
        // attach to a crash captured after consent is re-granted (GDPR scoping).
        breadcrumbs_.clear();
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::start_session() {
    // One-way idempotent latch (Unity 0.15 parity): the first call releases the
    // held heartbeats and batch drains; repeats are no-ops. Kick the heartbeat
    // loop so the first beat goes out promptly (not up to an interval late) and
    // wake the worker so batches buffered while deferred drain now.
    if (start_gate_.set()) {
        {
            const std::lock_guard<std::mutex> lock(heartbeat_mutex_);
            heartbeat_kick_ = true;
        }
        heartbeat_cv_.notify_all();
        if (worker_) {
            worker_->wake();
        }
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::set_user_metadata(const char *const *keys, const char *const *values,
                                           std::size_t count) {
    if (count > 0 && (keys == nullptr || values == nullptr)) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    // REPLACE semantics: the whole map is swapped every call (NULL/0 clears),
    // clamped to the wire contract (16 pairs, key 64, value 512). The heartbeat
    // change detection against the last acked map decides when it ships.
    UserMetadataEntries entries = clamp_user_metadata(keys, values, count);
    const std::lock_guard<std::mutex> lock(metadata_mutex_);
    user_metadata_ = std::move(entries);
    return TOMBSTONE_OK;
}

void Client::report_frame(double frame_ms) {
    if (!capture_allowed()) {
        return;  // fail-soft: nothing accumulates before init or while consent is off
    }
    frame_stats_.sample(frame_ms);
}

tombstone_result Client::set_environment(const char *environment) {
    // Null/empty is rejected and the current value retained — the environment can
    // never be silently blanked (a bad call must not mis-tag the whole session).
    if (environment == nullptr || environment[0] == '\0') {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    const std::lock_guard<std::mutex> lock(match_mutex_);
    environment_ = std::string{utf8_safe_truncate(environment, limits::environment)};
    return TOMBSTONE_OK;
}

tombstone_result Client::set_server_info(const char *region, const char *hostname) {
    // NULL keeps a label unchanged; "" clears it (a cleared label is omitted on the wire).
    const std::lock_guard<std::mutex> lock(match_mutex_);
    if (region != nullptr) {
        region_ = std::string{utf8_safe_truncate(region, limits::region)};
    }
    if (hostname != nullptr) {
        hostname_ = std::string{utf8_safe_truncate(hostname, limits::hostname)};
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::mark_dedicated_server(const char *server_id, const char *region,
                                               const char *hostname) {
    // Declare this process a dedicated server WITHOUT requiring a match: flip role to
    // "server" and set the id. NULL/empty server_id keeps the existing id (never blanks);
    // region/hostname are optional and NULL/empty keeps the current value.
    const std::lock_guard<std::mutex> lock(match_mutex_);
    role_ = "server";
    if (server_id != nullptr && server_id[0] != '\0') {
        server_id_ = std::string{utf8_safe_truncate(server_id, limits::server_id)};
    }
    if (region != nullptr && region[0] != '\0') {
        region_ = std::string{utf8_safe_truncate(region, limits::region)};
    }
    if (hostname != nullptr && hostname[0] != '\0') {
        hostname_ = std::string{utf8_safe_truncate(hostname, limits::hostname)};
    }
    return TOMBSTONE_OK;
}

tombstone_result Client::set_device(const tombstone_device_t *device) {
    DevicePayload copy;  // NULL -> clears the cached device
    if (device != nullptr) {
        const auto str = [](const char *v, std::size_t max) {
            return (v != nullptr) ? std::string{utf8_safe_truncate(v, max)} : std::string{};
        };
        copy.model = str(device->model, limits::device_model);
        copy.type = str(device->type, limits::device_type);
        copy.os = str(device->os, limits::device_os);
        copy.os_family = str(device->os_family, limits::device_os_family);
        copy.cpu = str(device->cpu, limits::device_cpu);
        copy.cpu_count = device->cpu_count;
        copy.ram_mb = device->ram_mb;
        copy.gpu = str(device->gpu, limits::device_gpu);
        copy.gpu_vendor = str(device->gpu_vendor, limits::device_gpu_vendor);
        copy.gpu_version = str(device->gpu_version, limits::device_gpu_version);
        copy.gpu_api = str(device->gpu_api, limits::device_gpu_api);
        copy.vram_mb = device->vram_mb;
        copy.screen = str(device->screen, limits::device_screen);
        copy.screen_dpi = device->screen_dpi;
        copy.refresh_rate = device->refresh_rate;
        copy.orientation = str(device->orientation, limits::device_orientation);
        copy.fullscreen = device->fullscreen != 0;
        copy.language = str(device->language, limits::device_language);
        copy.engine = str(device->engine, limits::device_engine);
        copy.scripting_backend = str(device->scripting_backend, limits::device_scripting_backend);
        copy.platform = str(device->platform, limits::device_platform);
    }
    {
        const std::lock_guard<std::mutex> lock(match_mutex_);
        device_ = std::move(copy);
    }
    // A changed device re-arms heartbeat delivery: the new specs ride beats until acked.
    device_sent_on_heartbeat_.store(false, std::memory_order_relaxed);
    device_carry_in_flight_.store(false, std::memory_order_relaxed);
    return TOMBSTONE_OK;
}

tombstone_result Client::set_match_context(const char *server_id, const char *match_id) {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    server_id_ = (server_id != nullptr)
                     ? std::string{utf8_safe_truncate(server_id, limits::server_id)}
                     : std::string{};
    match_id_ = (match_id != nullptr)
                    ? std::string{utf8_safe_truncate(match_id, limits::match_id)}
                    : std::string{};
    return TOMBSTONE_OK;
}

tombstone_result Client::start_match(char *out_match_id, std::size_t out_cap) {
    if (out_match_id == nullptr || out_cap == 0) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    const std::string id = random_session_id();  // 32 hex chars, same generator as session ids
    if (out_cap < id.size() + 1) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;  // buffer too small: cached context unchanged
    }
    {
        const std::lock_guard<std::mutex> lock(match_mutex_);
        role_ = "server";
        match_id_ = id;
    }
    id.copy(out_match_id, id.size());
    out_match_id[id.size()] = '\0';
    return TOMBSTONE_OK;
}

tombstone_result Client::end_match() {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    match_id_.clear();  // role + server id are retained across matches
    return TOMBSTONE_OK;
}

Client::MatchContext Client::current_match_context() const {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    return MatchContext{role_, server_id_, match_id_, environment_};
}

Client::ServerInfo Client::current_server_info() const {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    return ServerInfo{region_, hostname_};
}

DevicePayload Client::current_device() const {
    const std::lock_guard<std::mutex> lock(match_mutex_);
    return device_;
}

tombstone_result Client::flush(int timeout_ms) {
    if (!initialized_ || !worker_) {
        return TOMBSTONE_ERROR_NOT_INITIALIZED;
    }
    if (session_log_enabled_ && storage_available_) {
        session_log_.flush_now();
    }
    flush_all_batches();  // buffered events/metrics join the outbound queue
    const auto timeout = std::chrono::milliseconds{timeout_ms > 0 ? timeout_ms : 0};
    return worker_->flush(timeout) ? TOMBSTONE_OK : TOMBSTONE_ERROR_TIMEOUT;
}

void Client::maybe_flush_batch(Batch &batch, const char *path,
                               std::chrono::steady_clock::time_point now, bool force,
                               bool suppress_rtt) {
    if (!batch.has_items()) {
        return;  // cheap short-circuit: an empty idle loop allocates nothing (section 15)
    }
    std::optional<std::string> envelope = batch.drain_if_ready(now_iso8601_utc_ms(), now, force);
    if (!envelope.has_value()) {
        return;
    }
    UploadJob job;
    job.url = endpoint_ + path;
    job.body = std::move(*envelope);
    job.durability = Durability::persist_on_failure;  // retried with backoff in-session
    job.no_persist = true;  // a batch envelope is not a single-item sidecar
    job.sign_body = true;  // events:batch / metrics:batch are ingest POSTs — sign them (S3)
    job.suppress_rtt = suppress_rtt;  // the metrics batch's own upload must not emit an rtt metric
    // Record the flush time for diagnostics (K3); steady-clock ns, 0 means "never".
    last_flush_steady_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    worker_->enqueue(std::move(job));
}

void Client::drain_ready_batches() {
    if (!start_gate_.is_set()) {
        return;  // v0.7 send gate: drains held until start_session (items keep buffering)
    }
    const auto now = std::chrono::steady_clock::now();
    maybe_flush_batch(event_batch_, events_batch_path, now, /*force=*/false, /*suppress_rtt=*/false);
    maybe_flush_batch(metric_batch_, metrics_batch_path, now, /*force=*/false, /*suppress_rtt=*/true);
}

void Client::flush_all_batches() {
    if (!start_gate_.is_set()) {
        // Send gate held: even the quit/pre-crash force-drain ships nothing —
        // the game explicitly asked for silence until start_session (crash/bug
        // report bodies themselves are exempt and already enqueued upstream).
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    maybe_flush_batch(event_batch_, events_batch_path, now, /*force=*/true, /*suppress_rtt=*/false);
    maybe_flush_batch(metric_batch_, metrics_batch_path, now, /*force=*/true, /*suppress_rtt=*/true);
}

}  // namespace tombstone
