#pragma once
#include <cwchar>
#include "shell_core.h"

namespace WindowTitleFrame {
inline constexpr uint16_t Kind = 1;
inline constexpr size_t MaxUnits = 32768;

struct Payload {
    const void* data;
    size_t bytes;
};

// The ordered window-title frame borrows UTF-16 text without its terminator.
// The caller chooses its scan bound and keeps its own refusal policy.
inline Payload Encode(const wchar_t* title, size_t scanLimit) noexcept {
    return {title, wcsnlen(title,scanLimit)*sizeof(wchar_t)};
}
}
