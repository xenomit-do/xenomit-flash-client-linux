#include "swf_bridge.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xenomit_flash_inject input.swf output.swf\n");
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input || bytes.empty()) {
        std::fprintf(stderr, "could not read %s\n", argv[1]);
        return 2;
    }
    if (bytes.size() > 64u * 1024u * 1024u) {
        std::fprintf(stderr, "SWF is too large\n");
        return 2;
    }
    auto patched = swf_bridge::inject(bytes.data(), bytes.size());
    if (!patched.ok) {
        std::fprintf(stderr, "%s\n", patched.error.c_str());
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(patched.swf.data()), static_cast<std::streamsize>(patched.swf.size()));
    if (!output) {
        std::fprintf(stderr, "could not write %s\n", argv[2]);
        return 2;
    }
    std::fprintf(stderr, "[xenomit-flash] bridge injected compression=%s bytes=%zu\n", patched.compression.c_str(),
                 patched.swf.size());
    return 0;
}
