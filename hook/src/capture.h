#pragma once
#include <cstdint>

// Installs the verify and free-chunk trampolines at `base`, the load address
// of the pinned plugin. Returns false when a hook cannot be installed.
bool capture_install_runtime_hooks(uintptr_t base);
