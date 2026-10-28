#pragma once
#include <cstddef>
#include <cstdint>

namespace hlaftp::detail {
enum class WriteResult { Written, Changed, Fault };
// Separate SEH frame lets the caller restore page protection on every return.
WriteResult GuardedWrite(uint8_t* address, const uint8_t* before, const uint8_t* after, size_t size) noexcept;
}
