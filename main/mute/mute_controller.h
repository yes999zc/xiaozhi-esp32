#ifndef MUTE_CONTROLLER_H
#define MUTE_CONTROLLER_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * B2 device side of the mute switch.
 *
 * The server is the single source of truth: this controller only ever mirrors
 * `muted` from `GET/POST /debug/mute` (4411, Bearer control token). It never
 * persists a muted boolean, so a reboot always re-reads the server state.
 *
 * The only things kept in NVS are the control-plane endpoint (origin) and the
 * token, both handed to the firmware by the server in the MCP `initialize`
 * capabilities (`capabilities.vision.url` / `.token`; the vision token defaults
 * to the control token). Those two values let the device poll before the user
 * ever opens a conversation on the next boot.
 *
 * One small task (6 KiB, priority 3) owns every HTTP request, so the LVGL task
 * and the main event loop never block on the network. Polling does not yield to
 * device states: an external CLI change must reach the screen within 3 s.
 */
enum class MuteSyncState {
    kNoEndpoint,  // no token yet: screen shows "待同步"
    kSyncing,     // a sync is on the wire (startup / after a CLI change)
    kSynced,      // last response matched the server
    kFailed,      // last request failed; `muted` is the last confirmed value
};

struct MuteStatus {
    bool muted = false;  // last-confirmed server value
    MuteSyncState state = MuteSyncState::kNoEndpoint;
    bool has_result = false;  // at least one server response was accepted
};

class MuteController {
public:
    MuteController();
    ~MuteController();

    MuteController(const MuteController&) = delete;
    MuteController& operator=(const MuteController&) = delete;

    using StateCallback = std::function<void(const MuteStatus& status)>;
    // Invoked (from this task) when three consecutive 401/403 make the stored
    // token useless (server rotated it or the IP changed). The application
    // reopens the WS once to receive a fresh MCP initialize; any mute hold
    // stays applied, so this only refreshes the control token.
    using TokenRefreshCallback = std::function<void()>;

    // Must be called before Start().
    void SetStateCallback(StateCallback callback);
    void SetTokenRefreshCallback(TokenRefreshCallback callback);

    void Start();
    void Stop();

    // `capabilities.vision.url` + `.token` from the server MCP initialize.
    // The URL path is dropped: only scheme://host[:port] is kept and
    // `/debug/mute` is appended.
    void SetEndpoint(const std::string& vision_url, const std::string& token);

    bool HasEndpoint() const;

    // Kick an immediate GET (boot / network reconnect / new endpoint).
    void OnNetworkReady();

    // Explicit user toggle. Returns false when there is no endpoint or a
    // request is already in flight.
    bool RequestToggle();

    // Reported by the application after the one-shot WS reopen triggered by
    // TokenRefreshCallback. `ok` only means a fresh channel/hello was obtained.
    void OnTokenRefreshFinished(bool ok);

    MuteStatus status() const;

private:
    enum class SyncResult {
        kOk,
        kNetwork,   // connect/timeout/5xx - retry with backoff
        kAuth,      // 401/403 - token may be stale
        kProtocol,  // 200 but unparsable / no boolean `muted`
    };

    static void TaskTrampoline(void* arg);
    void TaskLoop();
    SyncResult PerformSync(bool do_toggle, bool target_muted, bool* muted_out);
    void PublishState();
    static int64_t NowMs();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    TaskHandle_t task_handle_ = nullptr;
    bool stopping_ = false;

    StateCallback state_callback_;
    TokenRefreshCallback token_refresh_callback_;

    // Endpoint (NVS-persisted; never includes the muted flag).
    std::string base_url_;
    std::string token_;
    bool has_endpoint_ = false;

    // Server truth as last seen. Only written from a successful response.
    bool last_confirmed_muted_ = false;
    bool have_result_ = false;
    MuteSyncState state_ = MuteSyncState::kNoEndpoint;

    int fail_streak_ = 0;
    bool trigger_ = false;         // immediate GET requested
    bool toggle_pending_ = false;  // explicit POST requested
    bool toggle_in_flight_ = false;
    bool retry_toggle_ = false;        // a failed POST is retried as a POST
    int64_t next_due_ms_ = INT64_MAX;  // scheduled poll (ms, monotonic)

    // Token-rotation recovery (401/403 x3; one WS reopen).
    uint32_t auth_fail_streak_ = 0;
    bool token_refresh_in_flight_ = false;
    bool token_refresh_done_ = false;  // one WS reopen until a sync succeeds again
    int64_t token_refresh_started_ms_ = 0;
    bool stack_mark_logged_ = false;
};

#endif  // MUTE_CONTROLLER_H
