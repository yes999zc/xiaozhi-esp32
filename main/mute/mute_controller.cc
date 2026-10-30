#include "mute_controller.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <board.h>
#include <cJSON.h>
#include <http.h>
#include <settings.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>

#define TAG "Mute"

namespace {

// The server answers on the LAN; 5 s is the same order as the OTA HTTP calls.
constexpr int kHttpTimeoutMs = 5000;
// Poll cadence (B2 acceptance #6: an external CLI change reaches the screen
// within 3 s). Muted 1 s / unmuted 2 s, so either direction is seen in <=2 s
// and the UI repaint is well under 1 s; the toggle recheck below is 300 ms.
// Polling deliberately does not yield to Speaking/Notifying/Upgrading: the
// HTTP task is independent (6 KiB) and the 3 s promise has to hold.
constexpr int64_t kPollMutedMs = 1000;
constexpr int64_t kPollUnmutedMs = 2000;
// A toggle is re-read 300 ms after the POST response (the POST already returns
// the new state; this catches a second writer).
constexpr int64_t kToggleRecheckMs = 300;
// Failed requests (GET and POST alike) back off 2 -> 5 -> 10 -> 30 s.
constexpr int64_t kFailBackoffMs[] = {2000, 5000, 10000, 30000};
constexpr int64_t kIdleWaitMs = 1000;
// A WS reopen triggered by the token-rotation recovery should answer within a
// few seconds; if the application never reports back, stop waiting.
constexpr int64_t kTokenRefreshWatchdogMs = 10000;
// Three consecutive 401/403 mean the stored token/IP is stale. The dangerous
// case is while muted (the device cannot be unmuted), but recovery is valid in
// any state because the reopen is gated to Idle and keeps capture/uplink off.
constexpr uint32_t kAuthFailuresBeforeRefresh = 3;

// "http://<server-host>:4411/xiaozhi/vision" -> "http://<server-host>:4411".
// Only the origin is kept: the device appends /debug/mute itself.
std::string OriginOf(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }
    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return url;
    }
    return url.substr(0, path_start);
}

}  // namespace

MuteController::MuteController() = default;

MuteController::~MuteController() { Stop(); }

void MuteController::SetStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callback_ = std::move(callback);
}

void MuteController::SetTokenRefreshCallback(TokenRefreshCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    token_refresh_callback_ = std::move(callback);
}

void MuteController::Start() {
    std::string endpoint_for_log;
    bool token_stored = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_handle_ != nullptr) {
            return;
        }
        // Only the endpoint is persisted; `muted` always comes from the server.
        Settings settings("mute", false);
        base_url_ = settings.GetString("base_url");
        token_ = settings.GetString("token");
        has_endpoint_ = !base_url_.empty() && !token_.empty();
        state_ = has_endpoint_ ? MuteSyncState::kSyncing : MuteSyncState::kNoEndpoint;
        next_due_ms_ = INT64_MAX;
        stopping_ = false;
        // Copy for logging outside the lock: SetEndpoint can run concurrently
        // (protocol task) once the controller is alive.
        endpoint_for_log = base_url_;
        token_stored = !token_.empty();
    }
    ESP_LOGI(TAG, "start: endpoint=%s token=%s",
             endpoint_for_log.empty() ? "(none)" : endpoint_for_log.c_str(),
             token_stored ? "stored" : "missing");

    // HTTP + cJSON live on this task; 6 KiB keeps the parse of a small JSON
    // response away from the stack edge (was 4 KiB in the first draft).
    const BaseType_t created =
        xTaskCreate(&MuteController::TaskTrampoline, "mute_ctrl", 6144, this, 3, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create mute task (%d), syncing disabled",
                 static_cast<int>(created));
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle_ = nullptr;
        state_ = MuteSyncState::kFailed;
    }
    PublishState();
}

void MuteController::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_handle_ == nullptr) {
            return;
        }
        stopping_ = true;
        cv_.notify_all();
    }
    // Shutdown-only path (Application is a singleton): wait for the task to
    // exit. A sync in flight can block up to kHttpTimeoutMs (5 s), so allow a
    // little more than that instead of the old 2 s window.
    for (int i = 0; i < 700; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_handle_ == nullptr) {
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "mute task did not stop within 7 s (HTTP in flight); process is shutting down");
}

void MuteController::SetEndpoint(const std::string& vision_url, const std::string& token) {
    const std::string base_url = OriginOf(vision_url);
    if (base_url.empty() || token.empty()) {
        ESP_LOGW(TAG, "ignoring endpoint: url=%s token=%s",
                 vision_url.empty() ? "(empty)" : vision_url.c_str(),
                 token.empty() ? "missing" : "present");
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = (base_url != base_url_) || (token != token_);
        if (changed) {
            base_url_ = base_url;
            token_ = token;
            has_endpoint_ = true;
            Settings settings("mute", true);
            settings.SetString("base_url", base_url_);
            settings.SetString("token", token_);
            state_ = MuteSyncState::kSyncing;
            fail_streak_ = 0;
            next_due_ms_ = INT64_MAX;
            // A fresh endpoint/token is a new chance for the recovery path.
            auth_fail_streak_ = 0;
            token_refresh_done_ = false;
            token_refresh_in_flight_ = false;
        }
        // A fresh WS session is also a good moment to re-read the truth, even
        // when the endpoint did not change.
        trigger_ = true;
        cv_.notify_all();
    }

    ESP_LOGI(TAG, "endpoint %s%s", base_url.c_str(), changed ? " (updated, persisted)" : "");
    if (changed) {
        PublishState();
    }
}

bool MuteController::HasEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_endpoint_;
}

void MuteController::OnNetworkReady() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_endpoint_) {
            return;
        }
        trigger_ = true;
        cv_.notify_all();
    }
    ESP_LOGI(TAG, "network ready: sync requested");
}

bool MuteController::RequestToggle() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_endpoint_ || toggle_pending_ || toggle_in_flight_) {
            return false;
        }
        toggle_pending_ = true;
        cv_.notify_all();
    }
    return true;
}

void MuteController::OnTokenRefreshFinished(bool ok) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_refresh_in_flight_ = false;
        auth_fail_streak_ = 0;
        if (ok) {
            // The fresh channel may have delivered a new endpoint (SetEndpoint
            // resets this again); until a sync succeeds, do not loop reopens.
            token_refresh_done_ = true;
            // Re-read immediately: the MCP initialize can arrive after the
            // channel open returns, and waiting for the normal cadence would
            // leave the screen on 同步失败 for up to 30 s after a failed poll.
            trigger_ = true;
            next_due_ms_ = NowMs();
            cv_.notify_all();
        }
    }
    ESP_LOGI(TAG, "token refresh finished: channel=%s", ok ? "open" : "failed");
}

MuteStatus MuteController::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return MuteStatus{last_confirmed_muted_, state_, have_result_};
}

void MuteController::PublishState() {
    MuteStatus status;
    StateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = MuteStatus{last_confirmed_muted_, state_, have_result_};
        callback = state_callback_;
    }
    if (callback) {
        callback(status);
    }
}

void MuteController::TaskTrampoline(void* arg) {
    static_cast<MuteController*>(arg)->TaskLoop();
    vTaskDelete(nullptr);
}

void MuteController::TaskLoop() {
    while (true) {
        bool do_toggle = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopping_) {
                    task_handle_ = nullptr;
                    return;
                }
                const int64_t now_ms = NowMs();
                // If the application never reports a refresh result, do not
                // wait forever before the next auth failure can retry.
                if (token_refresh_in_flight_ &&
                    now_ms - token_refresh_started_ms_ > kTokenRefreshWatchdogMs) {
                    token_refresh_in_flight_ = false;
                }
                if (toggle_pending_ && has_endpoint_) {
                    toggle_pending_ = false;
                    retry_toggle_ = false;
                    toggle_in_flight_ = true;
                    do_toggle = true;
                    break;
                }
                if (has_endpoint_ && retry_toggle_ && now_ms >= next_due_ms_) {
                    retry_toggle_ = false;
                    toggle_in_flight_ = true;
                    do_toggle = true;
                    break;
                }
                if (has_endpoint_ && (trigger_ || now_ms >= next_due_ms_)) {
                    trigger_ = false;
                    do_toggle = false;
                    break;
                }
                int64_t wait_ms = kIdleWaitMs;
                if (has_endpoint_ && next_due_ms_ != INT64_MAX) {
                    wait_ms = std::clamp<int64_t>(next_due_ms_ - now_ms, 50, kIdleWaitMs);
                }
                cv_.wait_for(lock, std::chrono::milliseconds(wait_ms));
            }
        }

        if (!do_toggle) {
            // A toggle may have arrived while waiting; it wins over the GET.
            std::lock_guard<std::mutex> lock(mutex_);
            if (toggle_pending_ && has_endpoint_) {
                toggle_pending_ = false;
                retry_toggle_ = false;
                toggle_in_flight_ = true;
                do_toggle = true;
            }
        }

        bool target_muted = false;
        bool previous_muted = false;
        bool had_result = false;
        MuteSyncState previous_state = MuteSyncState::kNoEndpoint;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!has_endpoint_) {
                toggle_in_flight_ = false;
                continue;
            }
            target_muted = !last_confirmed_muted_;
            previous_muted = last_confirmed_muted_;
            had_result = have_result_;
            previous_state = state_;
        }

        bool muted_out = false;
        const SyncResult result = PerformSync(do_toggle, target_muted, &muted_out);

        int64_t delay_ms = 0;
        bool publish = false;
        bool request_token_refresh = false;
        bool log_stack_mark = false;
        uint32_t auth_fail_snapshot = 0;
        TokenRefreshCallback refresh_cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (do_toggle) {
                toggle_in_flight_ = false;
            }
            if (result == SyncResult::kOk) {
                const bool changed = !have_result_ || muted_out != last_confirmed_muted_;
                have_result_ = true;
                last_confirmed_muted_ = muted_out;
                const bool state_changed = state_ != MuteSyncState::kSynced;
                state_ = MuteSyncState::kSynced;
                fail_streak_ = 0;
                auth_fail_streak_ = 0;
                token_refresh_done_ = false;
                token_refresh_in_flight_ = false;
                // 300 ms after a toggle the GET confirms the written value.
                delay_ms =
                    do_toggle ? kToggleRecheckMs : (muted_out ? kPollMutedMs : kPollUnmutedMs);
                publish = changed || state_changed || do_toggle;
                if (!stack_mark_logged_) {
                    stack_mark_logged_ = true;
                    log_stack_mark = true;
                }
            } else {
                state_ = MuteSyncState::kFailed;
                ++fail_streak_;
                if (do_toggle) {
                    // Retry the POST (a GET would only re-read the old value)
                    // on the same 2/5/10/30 s backoff.
                    retry_toggle_ = true;
                    const size_t index =
                        std::min<size_t>(fail_streak_ - 1, std::size(kFailBackoffMs) - 1);
                    delay_ms = kFailBackoffMs[index];
                } else {
                    const size_t index =
                        std::min<size_t>(fail_streak_ - 1, std::size(kFailBackoffMs) - 1);
                    delay_ms = kFailBackoffMs[index];
                }
                if (result == SyncResult::kAuth) {
                    // The dangerous case is "muted + stale token" (the device
                    // looks deaf and cannot be unmuted), but a stale token is
                    // just as broken when never synced, e.g. after a reboot
                    // with a rotated token while the server is still muted.
                    // The reopen below is safe in any state: it is gated to
                    // Idle and never enables capture/uplink.
                    ++auth_fail_streak_;
                    if (auth_fail_streak_ >= kAuthFailuresBeforeRefresh && !token_refresh_done_ &&
                        !token_refresh_in_flight_) {
                        if (token_refresh_callback_) {
                            token_refresh_in_flight_ = true;
                            token_refresh_started_ms_ = NowMs();
                            request_token_refresh = true;
                            refresh_cb = token_refresh_callback_;
                        } else {
                            // Nothing can perform the reopen; do not keep retrying
                            // the same stale token forever.
                            token_refresh_done_ = true;
                        }
                    }
                } else if (result != SyncResult::kAuth) {
                    auth_fail_streak_ = 0;
                }
                auth_fail_snapshot = auth_fail_streak_;
                publish = true;
            }
            next_due_ms_ = NowMs() + delay_ms;
        }

        if (result == SyncResult::kOk) {
            if (do_toggle || muted_out != previous_muted || !had_result ||
                previous_state != MuteSyncState::kSynced) {
                ESP_LOGI(TAG, "sync ok: muted=%d via %s (next in %lld ms)", muted_out,
                         do_toggle ? "POST" : "GET", (long long)delay_ms);
            } else {
                ESP_LOGD(TAG, "poll ok: muted=%d (next in %lld ms)", muted_out,
                         (long long)delay_ms);
            }
        } else {
            if (result == SyncResult::kAuth) {
                ESP_LOGW(TAG,
                         "sync auth failure via %s: last-confirmed muted=%d, auth_fail=%u, "
                         "retry in %lld ms",
                         do_toggle ? "POST" : "GET", previous_muted, (unsigned)auth_fail_snapshot,
                         (long long)delay_ms);
            } else {
                ESP_LOGW(TAG, "sync failed(%s) via %s: last-confirmed muted=%d, retry in %lld ms",
                         result == SyncResult::kNetwork ? "network" : "protocol",
                         do_toggle ? "POST" : "GET", previous_muted, (long long)delay_ms);
            }
        }
        if (request_token_refresh) {
            // 401/403 x3: the stored token/IP may be stale. The application
            // reopens the WS once, which re-delivers the MCP capabilities
            // (vision.url/token). Any mute hold stays applied.
            ESP_LOGW(TAG,
                     "auth failures (last-confirmed muted=%d) -> requesting one WS token "
                     "refresh",
                     last_confirmed_muted_ ? 1 : 0);
            refresh_cb();
        }
        if (log_stack_mark) {
            ESP_LOGI(TAG, "task stack high-water mark=%u bytes (free, after first sync)",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        if (publish) {
            PublishState();
        }
    }
}

MuteController::SyncResult MuteController::PerformSync(bool do_toggle, bool target_muted,
                                                       bool* muted_out) {
    std::string base_url;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        base_url = base_url_;
        token = token_;
    }
    if (base_url.empty() || token.empty()) {
        return SyncResult::kNetwork;
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        return SyncResult::kNetwork;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "failed to create HTTP client");
        return SyncResult::kNetwork;
    }

    const std::string url = base_url + "/debug/mute";
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", "application/json");

    if (do_toggle) {
        // Explicit boolean, never a toggle/ops verb (the server validates a
        // JSON boolean and answers with the new state).
        std::string body = std::string("{\"muted\":") + (target_muted ? "true" : "false") + "}";
        http->SetContent(std::move(body));
    }

    const char* method = do_toggle ? "POST" : "GET";
    if (auto opened = http->Open(method, url); !opened) {
        ESP_LOGW(TAG, "open %s failed: %s", url.c_str(), opened.error().ToString().c_str());
        return SyncResult::kNetwork;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGW(TAG, "status read failed: %s", status_code.error().ToString().c_str());
        http->Close();
        return SyncResult::kNetwork;
    }
    if (*status_code == 401 || *status_code == 403) {
        ESP_LOGW(TAG, "%s %s -> HTTP %d (auth)", method, url.c_str(), *status_code);
        http->Close();
        return SyncResult::kAuth;
    }
    if (*status_code != 200) {
        ESP_LOGW(TAG, "%s %s -> HTTP %d", method, url.c_str(), *status_code);
        http->Close();
        return SyncResult::kNetwork;
    }

    const std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "invalid JSON from %s", url.c_str());
        return SyncResult::kProtocol;
    }
    const cJSON* muted = cJSON_GetObjectItem(root, "muted");
    const bool parsed = cJSON_IsBool(muted);
    const bool value = parsed && cJSON_IsTrue(muted);
    cJSON_Delete(root);
    if (!parsed) {
        ESP_LOGW(TAG, "response has no boolean `muted`");
        return SyncResult::kProtocol;
    }
    *muted_out = value;
    return SyncResult::kOk;
}

int64_t MuteController::NowMs() { return esp_timer_get_time() / 1000; }
