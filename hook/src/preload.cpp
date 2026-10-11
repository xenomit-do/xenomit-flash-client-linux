#include "capture.h"
#include "offsets.h"
#include "sha256.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace {
using DlopenFn = void *(*)(const char *, int);

std::atomic<bool> g_armed{false};
std::mutex g_install_mu;

void log(const char *message) { std::fprintf(stderr, "[xenomit-flash] %s\n", message); }

uintptr_t plugin_base() {
    std::ifstream maps("/proc/self/maps");
    uintptr_t lowest = 0;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libpepflashplayer.so") == std::string::npos) continue;
        auto sep = line.find('-');
        if (sep == std::string::npos) continue;
        auto start = std::strtoull(line.c_str(), nullptr, 16);
        if (!lowest || start < lowest) lowest = start;
    }
    return lowest;
}

void arm(const char *filename) {
    if (g_armed.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_install_mu);
    if (g_armed.load(std::memory_order_relaxed)) return;
    auto hash = sha256_file(filename);
    if (hash != kPepperSha256) {
        log("Pepper plugin hash does not match the pinned runtime; native capture stays off");
        g_armed.store(true, std::memory_order_release);
        return;
    }
    auto base = plugin_base();
    if (!base) {
        log("Pepper plugin is loaded but its mapping was not found");
        return;
    }
    if (!capture_install_runtime_hooks(base)) {
        log("failed to install the Pepper game-thread trampoline");
        return;
    }
    g_armed.store(true, std::memory_order_release);
    log("Pepper hooks installed");
}
} // namespace

DlopenFn original_dlopen = nullptr;

__attribute__((constructor)) void resolve_dlopen() {
    original_dlopen = reinterpret_cast<DlopenFn>(dlsym(RTLD_NEXT, "dlopen"));
}

extern "C" __attribute__((visibility("default"))) void *dlopen(const char *filename, int flags) {
    static thread_local int depth = 0;
    if (!original_dlopen || depth) return original_dlopen ? original_dlopen(filename, flags) : nullptr;
    ++depth;
    void *handle = original_dlopen(filename, flags);
    if (handle && filename && std::strstr(filename, "libpepflashplayer.so")) arm(filename);
    --depth;
    return handle;
}
