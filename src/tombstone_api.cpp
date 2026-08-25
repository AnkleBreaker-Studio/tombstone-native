#include <tombstone/tombstone.h>

#include "client.h"
#include "payloads.h"
#include "preinit.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// Kept in lockstep with project(... VERSION ...) in CMakeLists.txt BY HAND, and that has failed TWICE:
// the changelog records this string pinned at "0.1.0" once and stuck at "0.8.0" once. Deriving it from
// PROJECT_VERSION is the real fix; until then, bump both or ship a third wrong version.
constexpr const char *sdk_version = "0.9.2";

// The process-wide client. Entry points snapshot the shared_ptr under the
// mutex and call outside it, so a long flush() neither blocks other calls nor
// races a concurrent shutdown (the instance stays alive until the call ends).
std::mutex g_client_mutex;
std::shared_ptr<tombstone::Client> g_client;

// Pre-init capture store (v0.7): buffered into while no client exists,
// replayed + cleared inside tombstone_init. Guarded by g_client_mutex — the
// store itself is deliberately unsynchronized (see preinit.h).
tombstone::PreInitStore g_preinit;

std::shared_ptr<tombstone::Client> client_snapshot() {
    const std::lock_guard<std::mutex> lock(g_client_mutex);
    return g_client;
}

/** Run `action` against the live client with the ABI no-throw guarantee. */
template <typename Action>
tombstone_result with_client(Action &&action) noexcept {
    try {
        const std::shared_ptr<tombstone::Client> client = client_snapshot();
        if (!client) {
            return TOMBSTONE_ERROR_NOT_INITIALIZED;
        }
        return action(*client);
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

/**
 * Run `action` against the live client, or `buffer` into the pre-init store
 * when none exists (v0.7 pre-init capture). Same no-throw guarantee. The
 * re-check under the mutex closes the init race: tombstone_init holds
 * g_client_mutex across init + replay + publish, so a concurrent call either
 * buffers before the replay reads the store, or lands on the published client.
 */
template <typename Action, typename Buffer>
tombstone_result with_client_or_buffer(Action &&action, Buffer &&buffer) noexcept {
    try {
        std::shared_ptr<tombstone::Client> client = client_snapshot();
        if (!client) {
            const std::lock_guard<std::mutex> lock(g_client_mutex);
            if (!g_client) {
                return buffer(g_preinit);
            }
            client = g_client;
        }
        return action(*client);
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

/**
 * Replay the pre-init store into the freshly initialized client — in order,
 * AFTER the options were applied, so an explicit pre-init Set* beats the
 * matching init option (Unity 0.9.8 clobber-fix parity). Last-write-wins state
 * first (consent / user / environment / metadata) so the replayed captures are
 * gated and stamped correctly; then breadcrumbs; then events + metrics in
 * their original combined order (replay-time timestamps — the pre-init window
 * is milliseconds in practice); finally the remembered start-session latch.
 * Every call is fail-soft: a rejected replay item must not fail init.
 */
void replay_preinit(tombstone::Client &client, const tombstone::PreInitStore &store) {
    if (store.has_consent()) {
        (void)client.set_consent(store.consent());
    }
    if (store.has_user()) {
        (void)client.set_user(store.user_id().c_str(), store.steam_id().c_str());
    }
    if (store.has_environment()) {
        (void)client.set_environment(store.environment().c_str());
    }
    if (store.has_user_metadata()) {
        std::vector<const char *> keys;
        std::vector<const char *> values;
        keys.reserve(store.user_metadata().size());
        values.reserve(store.user_metadata().size());
        for (const auto &[key, value] : store.user_metadata()) {
            keys.push_back(key.c_str());
            values.push_back(value.c_str());
        }
        (void)client.set_user_metadata(keys.data(), values.data(), keys.size());
    }
    for (const auto &crumb : store.breadcrumbs()) {
        (void)client.add_breadcrumb(static_cast<tombstone_level>(crumb.level),
                                    crumb.message.c_str());
    }
    for (const auto &track : store.tracks()) {
        if (track.is_metric) {
            (void)client.track_metric(track.name.c_str(), track.value, track.unit.c_str());
        } else {
            std::vector<const char *> keys;
            std::vector<const char *> values;
            keys.reserve(track.attributes.size());
            values.reserve(track.attributes.size());
            for (const auto &[key, value] : track.attributes) {
                keys.push_back(key.c_str());
                values.push_back(value.c_str());
            }
            (void)client.track_event(track.name.c_str(), keys.data(), values.data(),
                                     keys.size());
        }
    }
    if (store.start_session_requested()) {
        (void)client.start_session();
    }
}

}  // namespace

extern "C" {

tombstone_result tombstone_options_init(tombstone_options *options) {
    if (options == nullptr) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    options->endpoint = nullptr;
    options->token = nullptr;
    options->build_version = nullptr;
    options->data_dir = nullptr;
    options->environment = nullptr;  // unset -> server defaults to "production"
    options->heartbeat_interval_s = 0;  // 0 -> default (60s)
    options->enable_heartbeats = 1;
    options->enable_session_log = 1;
    options->enable_unclean_shutdown_detection = 1;
    options->consent_granted = 1;
    options->auto_start_session = 1;  // 0 = defer sending until tombstone_start_session()
    options->enable_rtt_metric = 1;
    options->enable_native_crash_handler = 0;  // EXPERIMENTAL, opt-in (v0.9)
    options->log_callback = nullptr;
    options->log_callback_user_data = nullptr;
    return TOMBSTONE_OK;
}

tombstone_result tombstone_init(const tombstone_options *options) {
    try {
        if (options == nullptr) {
            return TOMBSTONE_ERROR_INVALID_ARGUMENT;
        }
        std::shared_ptr<tombstone::Client> fresh;
        {
            const std::lock_guard<std::mutex> lock(g_client_mutex);
            if (g_client) {
                return TOMBSTONE_ERROR_ALREADY_INITIALIZED;
            }
            fresh = std::make_shared<tombstone::Client>();
            const tombstone_result result = fresh->init(*options);
            if (result != TOMBSTONE_OK) {
                return result;  // fresh is destroyed; nothing was published
            }
            // v0.7: replay pre-init captures (in order, AFTER the options were
            // applied) and clear the store, all before publishing — a concurrent
            // capture call blocks on g_client_mutex and then lands on g_client.
            replay_preinit(*fresh, g_preinit);
            g_preinit.clear();
            g_client = std::move(fresh);
        }
        return TOMBSTONE_OK;
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

tombstone_result tombstone_shutdown(void) {
    try {
        std::shared_ptr<tombstone::Client> retired;
        {
            const std::lock_guard<std::mutex> lock(g_client_mutex);
            retired.swap(g_client);
        }
        if (!retired) {
            return TOMBSTONE_OK;  // idempotent
        }
        retired.reset();  // ~Client: flush log, persist queue, remove marker
        return TOMBSTONE_OK;
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

tombstone_result tombstone_set_user(const char *user_id, const char *steam_id) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) { return client.set_user(user_id, steam_id); },
        [&](tombstone::PreInitStore &store) {
            // Last-write-wins; NULL clears exactly like the live call (which
            // clamps at replay). Empty string == cleared on the wire.
            store.set_user(user_id != nullptr ? user_id : "",
                           steam_id != nullptr ? steam_id : "");
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_set_consent(int granted) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) { return client.set_consent(granted != 0); },
        [&](tombstone::PreInitStore &store) {
            // Buffered last-write-wins; replayed AFTER options.consent_granted
            // is applied, so an explicit pre-init choice beats the init option.
            store.set_consent(granted != 0);
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_start_session(void) {
    return with_client_or_buffer(
        [](tombstone::Client &client) { return client.start_session(); },
        [](tombstone::PreInitStore &store) {
            store.request_start_session();  // remembered: the latch survives into init
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_set_environment(const char *environment) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) { return client.set_environment(environment); },
        [&](tombstone::PreInitStore &store) {
            if (environment == nullptr || environment[0] == '\0') {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;  // same contract as the live call
            }
            store.set_environment(environment);
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_set_user_metadata(const char *const *keys, const char *const *values,
                                             size_t count) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) {
            return client.set_user_metadata(keys, values, count);
        },
        [&](tombstone::PreInitStore &store) {
            if (count > 0 && (keys == nullptr || values == nullptr)) {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;
            }
            // Clamp at buffer time so a pre-init caller can't grow the store
            // unbounded; an empty result is a remembered CLEAR (replace semantics).
            store.set_user_metadata(tombstone::clamp_user_metadata(keys, values, count));
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_set_server_info(const char *region, const char *hostname) {
    return with_client(
        [&](tombstone::Client &client) { return client.set_server_info(region, hostname); });
}

tombstone_result tombstone_mark_dedicated_server(const char *server_id, const char *region,
                                                 const char *hostname) {
    return with_client([&](tombstone::Client &client) {
        return client.mark_dedicated_server(server_id, region, hostname);
    });
}

tombstone_result tombstone_set_device(const tombstone_device_t *device) {
    return with_client(
        [&](tombstone::Client &client) { return client.set_device(device); });
}

void tombstone_set_match_context(tombstone_handle * /*handle*/, const char *server_id,
                                 const char *match_id) {
    (void)with_client([&](tombstone::Client &client) {
        return client.set_match_context(server_id, match_id);
    });
}

tombstone_result tombstone_start_match(tombstone_handle * /*handle*/, char *out_match_id,
                                       size_t out_cap) {
    return with_client([&](tombstone::Client &client) {
        return client.start_match(out_match_id, out_cap);
    });
}

void tombstone_end_match(tombstone_handle * /*handle*/) {
    (void)with_client([&](tombstone::Client &client) { return client.end_match(); });
}

tombstone_result tombstone_add_breadcrumb(tombstone_level level, const char *message) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) { return client.add_breadcrumb(level, message); },
        [&](tombstone::PreInitStore &store) {
            if (message == nullptr || message[0] == '\0') {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;  // same contract as the live call
            }
            store.add_breadcrumb(static_cast<int>(level), message);
            return TOMBSTONE_OK;
        });
}

tombstone_result tombstone_track_event(const char *name, const char *const *keys,
                                       const char *const *values, size_t count) {
    return with_client_or_buffer(
        [&](tombstone::Client &client) {
            return client.track_event(name, keys, values, count);
        },
        [&](tombstone::PreInitStore &store) {
            if (name == nullptr || name[0] == '\0') {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;
            }
            if (count > 0 && (keys == nullptr || values == nullptr)) {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;
            }
            std::vector<std::pair<std::string, std::string>> attributes;
            attributes.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                if (keys[i] == nullptr || keys[i][0] == '\0') {
                    continue;  // mirrors the live call: skip unusable entries
                }
                attributes.emplace_back(keys[i], values[i] != nullptr ? values[i] : "");
            }
            (void)store.add_event(name, std::move(attributes));  // drop-newest at cap
            return TOMBSTONE_OK;
        });
}

void tombstone_track_metric(tombstone_handle * /*handle*/, const char *name, double value,
                            const char *unit) {
    (void)with_client_or_buffer(
        [&](tombstone::Client &client) { return client.track_metric(name, value, unit); },
        [&](tombstone::PreInitStore &store) {
            if (name == nullptr || name[0] == '\0' || !std::isfinite(value)) {
                return TOMBSTONE_ERROR_INVALID_ARGUMENT;  // same contract as the live call
            }
            (void)store.add_metric(name, value, unit != nullptr ? unit : "");
            return TOMBSTONE_OK;
        });
}

void tombstone_report_frame(double frame_ms) {
    // Never buffered: a pre-init frame window has no session to describe.
    (void)with_client([&](tombstone::Client &client) {
        client.report_frame(frame_ms);
        return TOMBSTONE_OK;
    });
}

tombstone_result tombstone_report_crash(const char *signature, const char *stack_hint,
                                        const char *stack_trace, int attach_log) {
    return with_client([&](tombstone::Client &client) {
        return client.report_crash(signature, stack_hint, stack_trace, attach_log != 0);
    });
}

tombstone_result tombstone_report_bug(const char *category, const char *message,
                                      int attach_log) {
    return with_client([&](tombstone::Client &client) {
        return client.report_bug(category, message, attach_log != 0);
    });
}

tombstone_result tombstone_log_line(tombstone_level level, const char *line) {
    return with_client([&](tombstone::Client &client) { return client.log_line(level, line); });
}

void tombstone_request_player_logs(tombstone_handle * /*handle*/, const char *target_type,
                                   const char *target_value, const char *reason) {
    (void)with_client([&](tombstone::Client &client) {
        return client.request_player_logs(target_type, target_value, reason);
    });
}

void tombstone_on_anomalous_disconnect(tombstone_handle * /*handle*/, const char *user_id,
                                       const char *reason) {
    (void)with_client([&](tombstone::Client &client) {
        const char *effective_reason =
            (reason != nullptr && reason[0] != '\0') ? reason : "anomalous disconnect";
        return client.request_player_logs("userId", user_id, effective_reason);
    });
}

void tombstone_set_sample_rate(tombstone_handle * /*handle*/, const char *name, double rate) {
    (void)with_client([&](tombstone::Client &client) {
        client.set_sample_rate(name, rate);
        return TOMBSTONE_OK;
    });
}

void tombstone_set_level(tombstone_handle * /*handle*/, const char *level_name) {
    (void)with_client([&](tombstone::Client &client) { return client.set_level(level_name); });
}

tombstone_result tombstone_diagnostics(tombstone_handle * /*handle*/,
                                       tombstone_diagnostics_t *out) {
    if (out == nullptr) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    *out = tombstone_diagnostics_t{};   // zero the snapshot first (also covers the not-init path)
    out->last_flush_age_seconds = -1.0;  // "no flush yet" sentinel
    return with_client([&](tombstone::Client &client) { return client.diagnostics(out); });
}

tombstone_result tombstone_flush(int timeout_ms) {
    return with_client([&](tombstone::Client &client) { return client.flush(timeout_ms); });
}

const char *tombstone_version(void) { return sdk_version; }

}  // extern "C"
