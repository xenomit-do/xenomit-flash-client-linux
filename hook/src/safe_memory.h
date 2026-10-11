#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>

// One game-thread capture owns the cache. A new capture never reuses old bytes.
// Each page is copied through the fault-safe reader, never dereferenced directly.
class SafeMemoryReader {
public:
    using Read = bool (*)(void *, uintptr_t, size_t);
    explicit SafeMemoryReader(Read read) : raw_(read) {}
    void begin() { active_ = true; invalidate(); }
    void end() { active_ = false; }
    void invalidate() { if (++epoch_ == 0) { for (auto &p : pages_) p.epoch = 0; ++epoch_; } }
    bool read(void *out, uintptr_t address, size_t size) {
        if (!active_) return raw_(out, address, size);
        if (address < 0x10000 || address > 0x00007fffffffffffULL - size) return false;
        auto dest = static_cast<unsigned char *>(out);
        while (size) {
            const auto base = address & ~uintptr_t(4095);
            const auto offset = address - base;
            const auto amount = std::min(size, size_t(4096 - offset));
            auto &page = pages_[((base >> 12) ^ (base >> 22)) & (pages_.size() - 1)];
            if (page.epoch != epoch_ || page.base != base) {
                page.epoch = 0;
                if (!raw_(page.bytes.data(), base, page.bytes.size())) {
                    // A readable fragment may share a page with inaccessible bytes.
                    if (!raw_(dest, address, amount)) return false;
                    dest += amount; address += amount; size -= amount;
                    continue;
                }
                page.base = base; page.epoch = epoch_;
            }
            std::memcpy(dest, page.bytes.data() + offset, amount);
            dest += amount; address += amount; size -= amount;
        }
        return true;
    }
private:
    struct Page { uintptr_t base = 0; uint64_t epoch = 0; std::array<unsigned char,4096> bytes; };
    Read raw_;
    bool active_ = false;
    uint64_t epoch_ = 0;
    std::array<Page,256> pages_;
};
class CaptureReadScope {
public:
    explicit CaptureReadScope(SafeMemoryReader &reader) : reader_(reader) { reader_.begin(); }
    ~CaptureReadScope() { reader_.end(); }
private:
    SafeMemoryReader &reader_;
};
