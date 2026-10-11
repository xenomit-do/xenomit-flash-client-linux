#include "capture.h"
#include "offsets.h"
#include "safe_memory.h"
#include "navigation_cache.h"

#include <subhook.h>

#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <vector>

namespace {
using Invoke = uintptr_t (*)(void *, uint32_t, uintptr_t *);
using VerifyFn = void (*)(uintptr_t, void *, uintptr_t, uintptr_t, void *, uintptr_t);
using FreeFn = void (*)(uintptr_t, uintptr_t);

constexpr uint8_t kPattern[] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
static_assert(sizeof(kPattern) == 92);

struct Header {
    uint32_t magic, version, seq, pad;
    uint64_t frame, timestamp;
    uint32_t capture_us;
    int32_t map_id;
    float width, height;
    int32_t hero_id;
    float hero_x, hero_y, dest_x, dest_y;
    uint32_t hero_flags;
    int32_t faction, clan;
    uint32_t entities, strings;
    uint64_t navigation_revision;
    uint32_t navigation_off, navigation_len;
    uint8_t reserved[24];
};
struct Entity {
    int32_t id;
    uint8_t kind, flags;
    uint16_t pad;
    float x, y, dx, dy;
    int32_t hp, max_hp, shield, max_shield, nano, max_nano, faction, clan, diplomacy, type;
    uint32_t name_off, name_len;
};
static_assert(sizeof(Header) == kHeaderLen && offsetof(Header, seq) == 8);
static_assert(sizeof(Entity) == kEntityLen);

struct MapRange {
    uintptr_t start, end;
    bool readable;
};

std::atomic<uintptr_t> g_application{0};
std::atomic<uintptr_t> g_screen{0};
std::atomic<uintptr_t> g_timer_env{0};
std::atomic<uintptr_t> g_method_info{0};
std::atomic<uintptr_t> g_env_proc{0};
std::atomic<uintptr_t> g_info_proc{0};
std::atomic<uintptr_t> g_invoker{0};
std::atomic<bool> g_timer_installed{false};
std::atomic<bool> g_in_proxy{false};

subhook::Hook *g_verify = nullptr;
subhook::Hook *g_free = nullptr;
uintptr_t g_scan = 0;
uint64_t g_retry_at = 0;
uint64_t g_next_arm = 0;
uint64_t g_next_capture = 0;
void publish_frame();
uint64_t g_frame = 0;
uint64_t g_revision = 0;
std::string g_published_navigation;
uint8_t *g_mapping = nullptr;
const char *g_failure = "native roots not discovered";
std::string g_last_logged;
std::vector<uint8_t> g_scan_buffer(1024 * 1024);
std::vector<Entity> g_records;
std::string g_names;

uint64_t monotonic_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000ull + uint64_t(ts.tv_nsec) / 1000000ull;
}
uint64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}
uint64_t unix_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000ull + uint64_t(ts.tv_nsec) / 1000000ull;
}

bool transfer(void *local, uintptr_t remote, size_t size, bool write) {
    if (!remote || remote < 0x10000 || remote > 0x00007fffffffffffULL - size) return false;
    iovec local_iov{local, size};
    iovec remote_iov{reinterpret_cast<void *>(remote), size};
    ssize_t n = write ? process_vm_writev(getpid(), &local_iov, 1, &remote_iov, 1, 0)
                      : process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
    return n == static_cast<ssize_t>(size);
}
bool raw_copy_from(void *to, uintptr_t from, size_t size) { return transfer(to, from, size, false); }
SafeMemoryReader g_memory(raw_copy_from);
const bool g_scalar_reads = std::getenv("XENOMIT_FLASH_SCALAR_READS") != nullptr;
bool copy_from(void *to, uintptr_t from, size_t size) { return g_scalar_reads ? raw_copy_from(to, from, size) : g_memory.read(to, from, size); }
bool copy_to(uintptr_t to, const void *from, size_t size) {
    g_memory.invalidate();
    return transfer(const_cast<void *>(from), to, size, true);
}
template <class T>
T read_as(uintptr_t address, bool &ok) {
    T value{};
    if (!copy_from(&value, address, sizeof(T))) ok = false;
    return value;
}
uintptr_t ptr_at(uintptr_t address, bool &ok) { return read_as<uintptr_t>(address, ok) & ~uintptr_t(7); }
bool write_ptr(uintptr_t address, uintptr_t value) { return copy_to(address, &value, sizeof(value)); }

bool utf8_append(std::string &out, uint32_t code) {
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    if (code < 0x80) {
        out.push_back(char(code));
    } else if (code < 0x800) {
        out.push_back(char(0xc0 | (code >> 6)));
        out.push_back(char(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
        out.push_back(char(0xe0 | (code >> 12)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(char(0x80 | (code & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (code >> 18)));
        out.push_back(char(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(char(0x80 | (code & 0x3f)));
    }
    return true;
}

bool decode_string(uintptr_t address, std::string &out) {
    out.clear();
    if (!address) return true;
    bool ok = true;
    auto length = read_as<uint32_t>(address + 32, ok);
    auto flags = read_as<uint32_t>(address + 36, ok);
    if (!ok || length > 1024 || ((flags & 6) >> 1) > 2) return false;
    auto data = read_as<uintptr_t>(address + 16, ok);
    if ((flags & 6) == 4) {
        auto master = ptr_at(address + 24, ok);
        auto master_flags = read_as<uint32_t>(master + 36, ok);
        auto master_length = read_as<uint32_t>(master + 32, ok);
        auto width = 1u + (flags & 1);
        auto buffer = read_as<uintptr_t>(master + 16, ok);
        uint64_t capacity = master_length;
        if ((master_flags & 6) == 0) {
            auto block = buffer & ~uintptr_t(4095);
            auto master_block = master & ~uintptr_t(4095);
            auto size = read_as<uint32_t>(block + 4, ok);
            auto gc = read_as<uintptr_t>(block + 8, ok);
            auto master_gc = read_as<uintptr_t>(master_block + 8, ok);
            if (!ok || !gc || gc != master_gc || (buffer & 7) || size < 8 || size > 8 * 1024 * 1024 ||
                (size & 7) || size % width || size / width < master_length)
                return false;
            if (size <= 2016) {
                auto items = read_as<uintptr_t>(block + 80, ok);
                if (!ok || items < block + 88 || items >= block + 4096 || buffer < items ||
                    (buffer - items) % size || buffer + size > block + 4096)
                    return false;
            }
            capacity = size / width;
        }
        if (!ok || (master_flags & 6) == 4 || (master_flags & 6) == 6 || (master_flags & 1) != (flags & 1) ||
            data % width || data / width > capacity || length > capacity - data / width)
            return false;
        data += buffer;
    }
    if (!ok) return false;
    std::array<uint16_t, 1024> units;
    if (flags & 1) {
        if (!copy_from(units.data(), data, length * 2)) return false;
    } else {
        std::array<uint8_t, 1024> narrow;
        if (length && !copy_from(narrow.data(), data, length)) return false;
        for (uint32_t i = 0; i < length; ++i) units[i] = narrow[i];
    }
    for (uint32_t i = 0; i < length;) {
        uint32_t code = units[i++];
        if (code >= 0xd800 && code <= 0xdbff) {
            if (i >= length) return false;
            uint32_t low = units[i++];
            if (low < 0xdc00 || low > 0xdfff) return false;
            code = 0x10000 + (((code - 0xd800) << 10) | (low - 0xdc00));
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            return false;
        }
        if (!utf8_append(out, code)) return false;
    }
    return true;
}

bool class_name(uintptr_t object, std::string &out) {
    bool ok = true;
    auto vtable = ptr_at(object + 16, ok);
    auto traits = ptr_at(vtable + 40, ok);
    auto name = ptr_at(traits + 144, ok);
    return ok && name && decode_string(name, out);
}
bool location(uintptr_t object, float &x, float &y) {
    bool ok = true;
    auto loc = ptr_at(object + 64, ok);
    auto a = read_as<double>(loc + 32, ok);
    auto b = read_as<double>(loc + 40, ok);
    if (!ok || a != a || b != b || a > 100000 || a < -100000 || b > 100000 || b < -100000) return false;
    x = static_cast<float>(a);
    y = static_cast<float>(b);
    return true;
}
bool bindable(uintptr_t owner, size_t offset, int32_t &value) {
    bool ok = true;
    auto slot = ptr_at(owner + offset, ok);
    if (!ok || !slot) return false;
    auto number = read_as<double>(slot + 56, ok);
    if (!ok || number != number || number < 0 || number > 2147483647) return false;
    value = static_cast<int32_t>(number);
    return true;
}

void note_failure(const char *message) {
    g_failure = message;
    if (g_last_logged == message) return;
    g_last_logged = message;
    std::fprintf(stderr, "[xenomit-flash] native capture: %s\n", message);
}

uintptr_t screen_of(uintptr_t app, uintptr_t *timer_env) {
    bool ok = true;
    auto main = ptr_at(app + 1344, ok);
    auto screen = ptr_at(main + 504, ok);
    std::string name;
    if (!ok || !class_name(screen, name) || name != "ScreenManager") return 0;
    auto event = ptr_at(screen + 200, ok);
    if (!ok || !class_name(event, name) || name != "EventManager") return 0;
    auto timer = ptr_at(ptr_at(screen + 16, ok) + 0x78 + 34 * 8, ok);
    auto getter = ptr_at(ptr_at(event + 16, ok) + 0x78 + 7 * 8, ok);
    auto move = ptr_at(ptr_at(event + 16, ok) + 0x78 + 10 * 8, ok);
    auto timer_info = ptr_at(timer + 16, ok);
    auto getter_info = ptr_at(getter + 16, ok);
    auto move_info = ptr_at(move + 16, ok);
    if (!ok || read_as<int32_t>(timer_info + 64, ok) != 9581 || read_as<int32_t>(getter_info + 64, ok) != 17459 ||
        read_as<int32_t>(move_info + 64, ok) != 17461 || ptr_at(timer_info + 48, ok) != ptr_at(getter_info + 48, ok) ||
        ptr_at(getter_info + 48, ok) != ptr_at(move_info + 48, ok) || !ok)
        return 0;
    if (timer_env) *timer_env = timer;
    return screen;
}

std::vector<MapRange> mappings() {
    std::vector<MapRange> ranges;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        auto sep = line.find('-');
        auto perm = line.find(' ');
        if (sep == std::string::npos || perm == std::string::npos || perm + 3 >= line.size()) continue;
        if (line.find("[vvar]") != std::string::npos || line.find("[vdso]") != std::string::npos ||
            line.find("[vsyscall]") != std::string::npos)
            continue;
        MapRange range;
        range.start = std::strtoull(line.c_str(), nullptr, 16);
        range.end = std::strtoull(line.c_str() + sep + 1, nullptr, 16);
        // AVM objects live in writable memory; skip code, assets and reserved V8 ranges.
        range.readable = line[perm + 1] == 'r' && line[perm + 2] == 'w';
        if (range.end > range.start) ranges.push_back(range);
    }
    return ranges;
}

uintptr_t discover(uintptr_t *timer_env) {
    if (monotonic_ms() < g_retry_at) return 0;
    auto ranges = mappings();
    for (int work = 0; work < 32; ++work) {
        auto range = std::find_if(ranges.begin(), ranges.end(), [](const MapRange &item) {
            return g_scan < item.end;
        });
        if (range == ranges.end()) {
            g_scan = 0;
            g_retry_at = monotonic_ms() + 2000;
            return 0;
        }
        if (g_scan < range->start) g_scan = range->start;
        if (!range->readable) {
            g_scan = range->end;
            continue;
        }
        auto amount = std::min(g_scan_buffer.size(), static_cast<size_t>(range->end - g_scan));
        if (copy_from(g_scan_buffer.data(), g_scan, amount)) {
            for (size_t i = 0; i + sizeof(kPattern) <= amount; i += 4) {
                if (g_scan_buffer[i] != 1 || std::memcmp(g_scan_buffer.data() + i, kPattern, sizeof(kPattern)))
                    continue;
                if (g_scan + i < 228) continue;
                auto app = g_scan + i - 228;
                if (auto screen = screen_of(app, timer_env)) {
                    g_application.store(app, std::memory_order_release);
                    return screen;
                }
            }
        }
        auto advanced = g_scan + amount;
        if (advanced > sizeof(kPattern) && advanced < range->end) advanced -= sizeof(kPattern);
        g_scan = advanced;
    }
    return 0;
}

bool map_frame() {
    if (g_mapping) return true;
    const char *path = std::getenv("XENOMIT_FLASH_STATE");
    if (!path || !path[0]) {
        note_failure("XENOMIT_FLASH_STATE is unset");
        return false;
    }
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        note_failure("live frame file is missing");
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kMappingLen)) {
        close(fd);
        note_failure("live frame file is the wrong size");
        return false;
    }
    auto view = mmap(nullptr, kMappingLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (view == MAP_FAILED) {
        note_failure("live frame file could not be mapped");
        return false;
    }
    g_mapping = static_cast<uint8_t *>(view);
    g_records.reserve(256);
    g_names.reserve(64 * 1024);
    return true;
}

bool overlaps(uintptr_t chunk, uintptr_t address) {
    if (!chunk || !address) return false;
    bool ok = true;
    auto block = chunk & ~uintptr_t(4095);
    auto size = read_as<uint32_t>(block + 4, ok);
    if (ok && size >= 8 && size <= 8 * 1024 * 1024 && (size % 8) == 0) {
        return address >= block && address < block + size;
    }
    return address >= chunk && address < chunk + 4096;
}

void restore_timer_procs() {
    auto env = g_timer_env.load(std::memory_order_acquire);
    auto info = g_method_info.load(std::memory_order_acquire);
    auto env_proc = g_env_proc.load(std::memory_order_acquire);
    auto info_proc = g_info_proc.load(std::memory_order_acquire);
    auto invoker = g_invoker.load(std::memory_order_acquire);
    if (env && env_proc) write_ptr(env + 8, env_proc);
    if (info && info_proc) write_ptr(info + 8, info_proc);
    if (info && invoker) write_ptr(info + 16, invoker);
}

void clear_roots() {
    g_timer_installed.store(false, std::memory_order_release);
    restore_timer_procs();
    g_application.store(0, std::memory_order_release);
    g_screen.store(0, std::memory_order_release);
    g_timer_env.store(0, std::memory_order_release);
    g_method_info.store(0, std::memory_order_release);
}

uintptr_t timer_proxy(void *env, uint32_t argc, uintptr_t *argv) {
    g_in_proxy.store(true, std::memory_order_release);
    auto info_proc = reinterpret_cast<Invoke>(g_info_proc.load(std::memory_order_acquire));
    auto invoker = reinterpret_cast<Invoke>(g_invoker.load(std::memory_order_acquire));
    auto info = g_method_info.load(std::memory_order_acquire);
    restore_timer_procs();
    uintptr_t result = 0;
    if (argv && info_proc && invoker) {
        result = (argv[0] & 7) ? invoker(env, argc, argv) : info_proc(env, argc, argv);
    }
    if (g_timer_installed.load(std::memory_order_acquire)) {
        bool ok = true;
        if (env) g_env_proc.store(read_as<uintptr_t>(reinterpret_cast<uintptr_t>(env) + 8, ok), std::memory_order_release);
        if (info) {
            g_info_proc.store(read_as<uintptr_t>(info + 8, ok), std::memory_order_release);
            g_invoker.store(read_as<uintptr_t>(info + 16, ok), std::memory_order_release);
        }
        if (ok) {
            auto proxy = reinterpret_cast<uintptr_t>(&timer_proxy);
            if (env) write_ptr(reinterpret_cast<uintptr_t>(env) + 8, proxy);
            if (info) {
                write_ptr(info + 8, proxy);
                write_ptr(info + 16, proxy);
            }
        }
        auto now = monotonic_ms();
        if (now >= g_next_capture) {
            // Suppress duplicate calls within a frame without dropping 60 Hz ticks
            // that fall just below 16 ms after integer clock rounding.
            g_next_capture = now + 8;
            publish_frame();
        }
    }
    g_in_proxy.store(false, std::memory_order_release);
    return result;
}

bool install_timer(uintptr_t timer_env) {
    if (g_timer_installed.load(std::memory_order_acquire)) return true;
    bool ok = true;
    auto screen = g_screen.load(std::memory_order_acquire);
    auto map = ptr_at(screen + 256, ok);
    auto hero = ptr_at(screen + 240, ok);
    std::string cls;
    if (!ok || !class_name(map, cls) || cls != "Map" ||
        !class_name(hero, cls) || cls != "Ship" || read_as<int32_t>(hero + 56, ok) <= 0)
        return false;
    auto info = ptr_at(timer_env + 16, ok);
    auto env_proc = read_as<uintptr_t>(timer_env + 8, ok);
    auto info_proc = read_as<uintptr_t>(info + 8, ok);
    auto invoker = read_as<uintptr_t>(info + 16, ok);
    if (!ok || !info || !env_proc || !info_proc || !invoker) return false;
    auto proxy = reinterpret_cast<uintptr_t>(&timer_proxy);
    if (env_proc == proxy || info_proc == proxy || invoker == proxy) return false;
    g_timer_env.store(timer_env, std::memory_order_release);
    g_method_info.store(info, std::memory_order_release);
    g_env_proc.store(env_proc, std::memory_order_release);
    g_info_proc.store(info_proc, std::memory_order_release);
    g_invoker.store(invoker, std::memory_order_release);
    if (!write_ptr(timer_env + 8, proxy) || !write_ptr(info + 8, proxy) || !write_ptr(info + 16, proxy)) {
        restore_timer_procs();
        return false;
    }
    g_timer_installed.store(true, std::memory_order_release);
    std::fprintf(stderr, "[xenomit-flash] game-thread timer armed\n");
    return true;
}

NavigationCache g_navigation;
std::string current_navigation(int32_t map_id) {
    const auto state = std::getenv("XENOMIT_FLASH_STATE");
    return g_navigation.get(state ? std::string(state) + ".navigation" : std::string(),
                            map_id, unix_ms(), monotonic_ms());
}

void publish_frame() {
    if (!map_frame()) return;
    auto started = monotonic_us();
    CaptureReadScope capture_reads(g_memory);
    uintptr_t timer_env = 0;
    auto app = g_application.load(std::memory_order_acquire);
    auto screen = app ? screen_of(app, &timer_env) : 0;
    if (!screen) {
        g_application.store(0, std::memory_order_release);
        screen = discover(&timer_env);
        if (!screen) {
            note_failure("native roots not discovered");
            return;
        }
    }
    if (timer_env) install_timer(timer_env);
    g_screen.store(screen, std::memory_order_release);
    bool ok = true;
    auto map = ptr_at(screen + 256, ok);
    auto hero = ptr_at(screen + 240, ok);
    std::string cls;
    Header header{};
    header.magic = kFrameMagic;
    header.version = kFrameVersion;
    header.map_id = read_as<int32_t>(map + 84, ok);
    auto width = read_as<int32_t>(map + 76, ok);
    auto height = read_as<int32_t>(map + 80, ok);
    header.width = static_cast<float>(width);
    header.height = static_cast<float>(height);
    header.hero_id = read_as<int32_t>(hero + 56, ok);
    if (!ok || !class_name(map, cls) || cls != "Map" || !class_name(hero, cls) || cls != "Ship" ||
        header.map_id < 1 || width < 100 || height < 100 || width > 100000 || height > 100000 ||
        header.hero_id <= 0 || !location(hero, header.hero_x, header.hero_y)) {
        note_failure("native root validation failed");
        return;
    }
    auto vector = ptr_at(map + 40, ok);
    auto table = ptr_at(vector + 48, ok);
    auto count = read_as<uint32_t>(vector + 56, ok);
    if (!ok || count > kMaxEntities) {
        note_failure("native entity vector rejected");
        return;
    }
    g_records.clear();
    g_names.clear();
    header.hero_flags = 1;
    for (uint32_t i = 0; i < count; ++i) {
        auto entity = ptr_at(table + 16 + i * 8, ok);
        Entity record{};
        record.id = read_as<int32_t>(entity + 56, ok);
        if (!ok || ptr_at(entity + 96, ok) != map || !location(entity, record.x, record.y) ||
            !class_name(entity, cls)) {
            note_failure("native entity validation failed");
            return;
        }
        std::string name;
        bool ship = cls == "Ship" || cls == "Pet";
        if (ship) {
            auto npc = read_as<int32_t>(entity + 112, ok);
            if (npc != 0 && npc != 1) {
                note_failure("native ship layout rejected");
                return;
            }
            record.kind = cls == "Pet" ? 4 : npc ? 3 : 2;
            record.flags = npc ? 1 : 0;
            auto health = ptr_at(entity + 184, ok);
            if (health) {
                if (!bindable(health, 48, record.hp) || !bindable(health, 56, record.max_hp) ||
                    !bindable(health, 64, record.shield) || !bindable(health, 72, record.max_shield) ||
                    !bindable(health, 80, record.nano) || !bindable(health, 88, record.max_nano)) {
                    note_failure("native health layout rejected");
                    return;
                }
                record.flags |= 8;
            }
            auto player = ptr_at(entity + 248, ok);
            if (player) {
                bindable(player, 40, record.clan);
                bindable(player, 48, record.diplomacy);
                bindable(player, 72, record.faction);
                auto username = ptr_at(player + 64, ok);
                if (username && !decode_string(ptr_at(username + 40, ok), name)) {
                    note_failure("native username rejected");
                    return;
                }
            }
            auto move = ptr_at(entity + 232, ok);
            auto dest = move ? ptr_at(move + 104, ok) : 0;
            if (dest) {
                auto dx = read_as<double>(dest + 32, ok);
                auto dy = read_as<double>(dest + 40, ok);
                if (dx != dx || dy != dy || dx > 100000 || dx < -100000 || dy > 100000 || dy < -100000) {
                    note_failure("native destination rejected");
                    return;
                }
                record.dx = static_cast<float>(dx);
                record.dy = static_cast<float>(dy);
                if (std::abs(record.dx - record.x) > 1 || std::abs(record.dy - record.y) > 1) record.flags |= 4;
            }
        } else if (cls == "Portal") {
            record.kind = 7;
            record.flags |= 16;
            record.type = read_as<int32_t>(entity + 120, ok);
        } else if (cls == "Box" || cls == "Ore") {
            record.kind = cls == "Box" ? 5 : 6;
            record.flags |= 32;
            if (!decode_string(ptr_at(entity + 160, ok), name)) {
                note_failure("native collectable hash rejected");
                return;
            }
        } else if (cls == "Station") {
            record.kind = 9;
        } else if (cls == "Headquarters" || cls == "Hangar" || cls == "RepairStation" || cls == "Refinery" ||
                   cls == "QuestGiver" || cls == "HomeBaseLaserTurret") {
            record.kind = 8;
        } else if (cls == "Mine") {
            record.kind = 11;
        } else if (cls == "Beacon") {
            record.kind = 10;
        }
        if (!ok) {
            note_failure("native pointer read failed");
            return;
        }
        if (entity == hero) {
            record.kind = 1;
            record.flags |= 2;
            header.faction = record.faction;
            header.clan = record.clan;
            header.dest_x = record.dx;
            header.dest_y = record.dy;
            if (record.flags & 4) header.hero_flags |= 2;
        }
        record.name_off = static_cast<uint32_t>(g_names.size());
        record.name_len = static_cast<uint32_t>(name.size());
        g_names += name;
        g_records.push_back(record);
    }
    if (std::none_of(g_records.begin(), g_records.end(), [](const Entity &record) { return record.kind == 1; })) {
        note_failure("hero absent from native entity vector");
        return;
    }
    const auto navigation = current_navigation(header.map_id);
    const auto navigation_len = navigation.size();
    header.navigation_off = static_cast<uint32_t>(g_names.size());
    header.navigation_len = static_cast<uint32_t>(navigation_len);
    if (g_names.size() + navigation_len > kMappingLen - kStringsOffset) {
        note_failure("native string table overflow");
        return;
    }
    if (navigation != g_published_navigation) {
        g_published_navigation = navigation;
        ++g_revision;
    }
    header.navigation_revision = g_revision;
    header.entities = static_cast<uint32_t>(g_records.size());
    header.strings = static_cast<uint32_t>(g_names.size() + navigation_len);
    header.frame = ++g_frame;
    header.timestamp = unix_ms();


    auto seq = reinterpret_cast<uint32_t *>(g_mapping + 8);
    auto next = __atomic_add_fetch(seq, 1, __ATOMIC_SEQ_CST);
    if ((next & 1) == 0) __atomic_add_fetch(seq, 1, __ATOMIC_SEQ_CST);
    if (!g_records.empty()) {
        std::memcpy(g_mapping + kHeaderLen, g_records.data(), g_records.size() * sizeof(Entity));
    }
    std::memcpy(g_mapping + kStringsOffset, g_names.data(), g_names.size());
    std::memcpy(g_mapping + kStringsOffset + g_names.size(), navigation.data(), navigation_len);
    header.capture_us = static_cast<uint32_t>(std::min<uint64_t>(monotonic_us() - started, 60000000));
    std::memcpy(g_mapping, &header, 8);
    std::memcpy(g_mapping + 12, reinterpret_cast<uint8_t *>(&header) + 12, sizeof(header) - 12);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_add_fetch(seq, 1, __ATOMIC_SEQ_CST);
    g_failure = "";
    g_last_logged.clear();
}

void verify_hook(uintptr_t self, void *method, uintptr_t signature, uintptr_t toplevel, void *abc, uintptr_t osr) {
    auto original = reinterpret_cast<VerifyFn>(g_verify->GetTrampoline());
    if (original) original(self, method, signature, toplevel, abc, osr);
    else {
        subhook::ScopedHookRemove remove(g_verify);
        reinterpret_cast<VerifyFn>(g_verify->GetSrc())(self, method, signature, toplevel, abc, osr);
    }
    if (g_timer_installed.load(std::memory_order_acquire)) return;
    auto now = monotonic_ms();
    if (now < g_next_arm) return;
    g_next_arm = now + 50;
    uintptr_t timer_env = 0;
    if (auto screen = discover(&timer_env)) {
        g_screen.store(screen, std::memory_order_release);
        if (timer_env) install_timer(timer_env);
    }
}

void free_hook(uintptr_t self, uintptr_t chunk) {
    if (!g_in_proxy.load(std::memory_order_acquire)) {
        auto app = g_application.load(std::memory_order_acquire);
        auto screen = g_screen.load(std::memory_order_acquire);
        auto timer = g_timer_env.load(std::memory_order_acquire);
        auto info = g_method_info.load(std::memory_order_acquire);
        if (overlaps(chunk, app) || overlaps(chunk, screen) || overlaps(chunk, timer) || overlaps(chunk, info)) {
            clear_roots();
        }
    }
    auto original = reinterpret_cast<FreeFn>(g_free->GetTrampoline());
    if (original) original(self, chunk);
    else {
        subhook::ScopedHookRemove remove(g_free);
        reinterpret_cast<FreeFn>(g_free->GetSrc())(self, chunk);
    }
}
} // namespace

bool capture_install_runtime_hooks(uintptr_t base) {
    if (g_verify) return g_timer_installed.load(std::memory_order_acquire) || g_verify->IsInstalled();
    g_verify = new subhook::Hook(reinterpret_cast<void *>(base + kVerifyJit), reinterpret_cast<void *>(verify_hook),
                                 subhook::HookFlag64BitOffset);
    g_free = new subhook::Hook(reinterpret_cast<void *>(base + kFreeChunk), reinterpret_cast<void *>(free_hook),
                               subhook::HookFlag64BitOffset);
    if (!g_free->Install()) {
        delete g_free;
        delete g_verify;
        g_free = nullptr;
        g_verify = nullptr;
        return false;
    }
    if (!g_verify->Install()) {
        g_free->Remove();
        delete g_free;
        delete g_verify;
        g_free = nullptr;
        g_verify = nullptr;
        return false;
    }
    return true;
}
