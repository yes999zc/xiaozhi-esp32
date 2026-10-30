#include "ui/ui_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Truncate a UTF-8 string to at most `max` bytes without splitting a code point.
void TrimIncompleteUtf8(char* text) {
    size_t n = strlen(text);
    if (n == 0) {
        return;
    }
    size_t i = n;
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            break;
        }
    }
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0) {
        need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
    }
    if (i + need > n) {
        text[i] = '\0';
    }
}

void CopyUtf8(char* dst, size_t max, const char* src) {
    if (max == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= max) {
        n = max - 1;
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}
}  // namespace

UiLog& UiLog::GetInstance() {
    static UiLog instance;
    return instance;
}

void UiLog::Add(UiLogLevel level, UiLogSource source, const char* tag, const char* fmt, ...) {
    const uint32_t now = NowMs();

    char tag_copy[kTagLen] = {};
    CopyUtf8(tag_copy, sizeof(tag_copy), tag != nullptr ? tag : "");

    char text[kTextLen] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    TrimIncompleteUtf8(text);

    std::lock_guard<std::mutex> lock(mutex_);

    // Merge identical consecutive lines first: repeat++ and refresh the time.
    // Rate limiting must only apply to *different* text, otherwise repeated
    // identical events would be counted as dropped instead of merged.
    if (count_ > 0) {
        const size_t last = (head_ + kCapacity - 1) % kCapacity;
        UiLogLine& prev = lines_[last];
        if (strcmp(prev.tag, tag_copy) == 0 && strcmp(prev.text, text) == 0) {
            if (prev.repeat < 255) {
                prev.repeat++;
            }
            prev.uptime_ms = now;
            return;
        }
    }

    // Rate limit per tag: 5 lines/s.
    TagRate* rate = nullptr;
    for (auto& entry : rates_) {
        if (entry.tag[0] == '\0' || strcmp(entry.tag, tag_copy) == 0) {
            rate = &entry;
            break;
        }
    }
    if (rate == nullptr) {
        rate = &rates_[0];
    }
    if (rate->tag[0] != '\0' && strcmp(rate->tag, tag_copy) == 0 &&
        now - rate->last_ms < 200) {
        dropped_++;
        return;
    }
    strncpy(rate->tag, tag_copy, sizeof(rate->tag) - 1);
    rate->last_ms = now;

    UiLogLine& line = lines_[head_];
    line.uptime_ms = now;
    line.seq = next_seq_++;
    line.level = level;
    line.source = source;
    memcpy(line.tag, tag_copy, sizeof(line.tag));
    memcpy(line.text, text, sizeof(line.text));
    line.repeat = 0;

    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) {
        count_++;
    }
}

size_t UiLog::Snapshot(UiLogLine* out, size_t max_lines) const {
    if (out == nullptr || max_lines == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = count_ > max_lines ? max_lines : count_;
    const size_t start = count_ - n;
    const size_t first = (head_ + kCapacity - count_) % kCapacity;
    for (size_t i = 0; i < n; ++i) {
        out[i] = lines_[(first + start + i) % kCapacity];
    }
    return n;
}

void UiLog::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = 0;
    head_ = 0;
    next_seq_ = 1;
    for (auto& rate : rates_) {
        rate = TagRate{};
    }
    dropped_ = 0;
}
