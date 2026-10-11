#pragma once
#include <string>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
struct NavigationHeader {
    uint32_t magic, version;
    uint64_t observed_at;
    int32_t map_id;
    uint32_t length;
};
static_assert(sizeof(NavigationHeader) == 24);
class NavigationCache {
    NavigationHeader header_{};
    std::string body_;
    uint64_t next_read_ = 0;
    static bool read_all(int fd, void *buffer, size_t length) {
        auto p = static_cast<char *>(buffer);
        while (length) {
            auto n = read(fd, p, length);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n; length -= static_cast<size_t>(n);
        }
        return true;
    }
public:
    std::string get(const std::string &file, int32_t map_id, uint64_t unix_now, uint64_t now) {
        if (now >= next_read_) {
            next_read_ = now + 100;
            body_.clear();
            int fd = file.empty() ? -1 : open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                NavigationHeader h{};
                struct stat st{};
                if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
                    read_all(fd, &h, sizeof(h)) && h.magic == 0x56414e58 && h.version == 1 &&
                    h.length > 1 && h.length <= 1024 * 1024 && st.st_size == off_t(sizeof(h) + h.length)) {
                    body_.resize(h.length);
                    if (read_all(fd, body_.data(), h.length)) header_ = h;
                    else body_.clear();
                }
                close(fd);
            }
        }
        if (!body_.empty() && header_.map_id == map_id &&
            header_.observed_at <= unix_now + 1000 &&
            unix_now <= header_.observed_at + 750) return body_;
        return "{\"map_id\":" + std::to_string(map_id) + ",\"geometry_ready\":false}";
    }

};
