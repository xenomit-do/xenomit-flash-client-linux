#pragma once
#include <cstddef>
#include <cstdint>

// Offsets into the pinned Pepper plugin. They are valid only for the build
// named by runtime/pepper.sha256 (PPAPI Flash 32.0.0.371).
inline constexpr char kPepperSha256[] =
    "7855aedc173b5aa79507825402feb02bf36ba012933526efb01dfb232248e32d";
inline constexpr char kPepperVersion[] = "32.0.0.371";

inline constexpr std::ptrdiff_t kVerifyJit = 0x2fb430;
inline constexpr std::ptrdiff_t kFreeChunk = 0xb8a100;

inline constexpr uint32_t kFrameMagic = 0x4D545358;
inline constexpr uint32_t kFrameVersion = 2;
inline constexpr std::size_t kHeaderLen = 128;
inline constexpr std::size_t kEntityLen = 72;
inline constexpr std::size_t kMaxEntities = 8192;
inline constexpr std::size_t kMappingLen = 2 * 1024 * 1024;
inline constexpr std::size_t kStringsOffset = kHeaderLen + kMaxEntities * kEntityLen;
