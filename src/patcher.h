#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hlaftp {
enum class PatchStatus { Found, Patched, AlreadyPatched, NotNeeded, PatternChanged, Error };
struct PatchSite { uint32_t rva; std::vector<uint8_t> before, after; };
struct FindResult { PatchStatus status; std::optional<PatchSite> site; std::string detail; };
FindResult FindPatchSite(const uint8_t* image, size_t imageSize);
FindResult ApplyPatch(uint8_t* image, size_t imageSize);
const char* StatusName(PatchStatus status);
}
