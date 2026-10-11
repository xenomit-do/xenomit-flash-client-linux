#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Lowercase hex SHA-256 of the file, or an empty string when it cannot be read.
std::string sha256_file(const char *path);
std::string sha256_bytes(const uint8_t *data, size_t len);
