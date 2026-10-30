#ifndef MUTE_UI_STATE_H
#define MUTE_UI_STATE_H

// B2 mute UI. The state always mirrors the server (`/debug/mute`); the device
// never shows "muted" from a local guess. Kept in its own header so both the
// display interface and Application can use it without pulling in lvgl.
enum class MuteUiState {
    kNoEndpoint,  // no control token yet -> "待同步"
    kSyncing,     // request on the wire -> "同步中…"
    kToggling,    // user just tapped the button -> "切换中…"
    kMuted,       // server-confirmed muted -> overlay + "解除静音"
    kUnmuted,     // server-confirmed unmuted
    kFailed,      // last request failed -> "同步失败", keep last confirmed visual
};

#endif  // MUTE_UI_STATE_H
