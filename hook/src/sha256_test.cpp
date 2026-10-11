#include "sha256.h"

#include <cstdio>
#include <cstring>

int main() {
    const auto abc = sha256_bytes(reinterpret_cast<const uint8_t *>("abc"), 3);
    const auto empty = sha256_bytes(nullptr, 0);
    if (abc != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::fprintf(stderr, "abc hash mismatch: %s\n", abc.c_str());
        return 1;
    }
    if (empty != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        std::fprintf(stderr, "empty hash mismatch: %s\n", empty.c_str());
        return 1;
    }
    return 0;
}
