#ifndef _UI_LOG_H_
#define _UI_LOG_H_

#include <cstddef>
#include <cstdint>
#include <mutex>

// Small, fixed-size event ring for the UI v2 information screen.
// Only curated events are written here; ESP_LOG output is not forwarded.
enum class UiLogLevel : uint8_t { kInfo = 0, kWarn = 1, kError = 2 };
enum class UiLogSource : uint8_t {
    kBoot = 0,
    kStt,
    kTts,
    kState,
    kLlm,
    kWake,
    kVad,
    kMute,
    kNet,
    kAudio,
};

struct UiLogLine {
    uint32_t uptime_ms = 0;
    uint16_t seq = 0;
    UiLogLevel level = UiLogLevel::kInfo;
    UiLogSource source = UiLogSource::kBoot;
    char tag[8] = {};
    char text[92] = {};
    uint8_t repeat = 0;  // consecutive duplicate count (rendered as "xN")
};

class UiLog {
public:
    static UiLog& GetInstance();

    // Thread-safe. `tag` is copied (max 7 chars); `text` is formatted and
    // truncated on a UTF-8 code point boundary.
    void Add(UiLogLevel level, UiLogSource source, const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

    // Copies the newest `max_lines` lines into `out` (oldest first). Returns
    // the number of copied lines. No sequence filtering: the UI always renders
    // the latest window, so a 16-bit seq wrap can never blank the screen.
    size_t Snapshot(UiLogLine* out, size_t max_lines) const;

    void Clear();

private:
    UiLog() = default;
    static constexpr size_t kCapacity = 10;
    static constexpr size_t kTagLen = sizeof(UiLogLine::tag);
    static constexpr size_t kTextLen = sizeof(UiLogLine::text);

    mutable std::mutex mutex_;
    UiLogLine lines_[kCapacity];
    size_t head_ = 0;  // next write slot
    size_t count_ = 0; // valid lines
    uint16_t next_seq_ = 1;
    // Per-tag rate limiting: at most 5 lines/s per tag.
    struct TagRate {
        char tag[kTagLen] = {};
        uint32_t last_ms = 0;
    };
    mutable TagRate rates_[12] = {};
    uint32_t dropped_ = 0;
};

#endif  // _UI_LOG_H_
