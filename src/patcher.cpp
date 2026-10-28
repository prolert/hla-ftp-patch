#include "patcher.h"
#include "pe_image.h"
#include "guarded_write.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hlaftp {
#ifdef _WIN32
namespace detail {
WriteResult GuardedWrite(uint8_t* address, const uint8_t* before, const uint8_t* after, size_t size) noexcept {
    bool started = false;
    __try {
        for (size_t i = 0; i < size; ++i) if (address[i] != before[i]) return WriteResult::Changed;
        started = true;
        std::memcpy(address, after, size);
        return WriteResult::Written;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // If another component removed the mapping mid-write, rollback is best
        // effort. The caller still flushes and restores protection after a fault.
        if (started) {
            __try { std::memcpy(address, before, size); }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        return WriteResult::Fault;
    }
}
}
#endif
namespace {
constexpr std::string_view Anchor = "cannot be less than the deactivation threshold";
int64_t Signed32(const uint8_t* bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (8*i);
    return value <= 0x7fffffffu ? static_cast<int64_t>(value) : static_cast<int64_t>(value)-0x100000000ll;
}
int64_t Signed8(uint8_t value) { return value < 128 ? value : static_cast<int64_t>(value)-256; }
bool ComissBefore(const uint8_t* image, size_t branch, size_t sectionStart) {
    // A register-register COMISS is three bytes, optionally preceded by a REX.
    if (branch < sectionStart+3 || image[branch-3] != 0x0f || image[branch-2] != 0x2f
        || (image[branch-1] & 0xc0) != 0xc0) return false;
    size_t instruction = branch-3;
    if (instruction > sectionStart && image[instruction-1] >= 0x40 && image[instruction-1] <= 0x4f) --instruction;
    if (instruction == sectionStart) return true;
    const uint8_t prefix = image[instruction-1];
    // 66 changes this to COMISD; repeat/lock prefixes are outside the supported pattern.
    return prefix != 0x66 && prefix != 0xf2 && prefix != 0xf3 && prefix != 0xf0;
}
FindResult Result(PatchStatus status, const char* detail) { return {status, std::nullopt, detail}; }
FindResult FindImpl(const uint8_t* image, size_t imageSize) {
    if (!image) return Result(PatchStatus::Error, "Null image");
    const auto pe = ParseMappedImage(std::span<const uint8_t>(image, imageSize));
    std::set<uint32_t> strings;
    bool unterminated = false;
    for (const auto& section : pe.sections) {
        if (section.executable || section.size < Anchor.size()) continue;
        const size_t start = section.rva;
        const size_t end = start+section.size;
        // Walk complete C strings, keeping their true start even when the anchor is a substring.
        size_t stringStart = start;
        while (stringStart < end) {
            const auto* nul = static_cast<const uint8_t*>(std::memchr(image+stringStart, 0, end-stringStart));
            const size_t stringEnd = nul ? static_cast<size_t>(nul-image) : end;
            const std::string_view text(reinterpret_cast<const char*>(image+stringStart), stringEnd-stringStart);
            if (text.find(Anchor) != std::string_view::npos) {
                if (!nul) unterminated = true;
                else strings.insert(static_cast<uint32_t>(stringStart));
            }
            if (!nul) break;
            stringStart = stringEnd+1;
        }
    }
    if (unterminated) return Result(PatchStatus::PatternChanged, "Anchor string has no terminator inside its section");
    if (strings.empty()) return Result(PatchStatus::NotNeeded, "Threshold validation anchor is absent");
    if (strings.size() != 1) return Result(PatchStatus::PatternChanged, "Multiple threshold validation strings");
    size_t lea = 0;
    const PeSection* code = nullptr;
    size_t references = 0;
    for (const auto& section : pe.sections) {
        if (!section.executable || section.size < 7) continue;
        const size_t end = static_cast<size_t>(section.rva)+section.size;
        for (size_t p = section.rva; p <= end-7; ++p) {
            if ((image[p] == 0x48 || image[p] == 0x4c) && image[p+1] == 0x8d && (image[p+2] & 0xc7) == 0x05
                && static_cast<int64_t>(p+7)+Signed32(image+p+3) == *strings.begin()) {
                ++references; lea = p; code = &section;
            }
        }
    }
    if (references != 1) return Result(PatchStatus::PatternChanged, "Expected exactly one executable RIP-relative LEA reference");
    const size_t start = (std::max)(static_cast<size_t>(code->rva), lea >= 0x80 ? lea-0x80 : 0);
    const size_t end = static_cast<size_t>(code->rva)+code->size;
    std::vector<std::pair<PatchSite, bool>> candidates;
    for (size_t p = start; p < lea; ++p) {
        if (!ComissBefore(image, p, code->rva)) continue;
        size_t length = 0;
        int64_t target = -1;
        bool patched = false;
        if (lea-p >= 6 && image[p] == 0x0f && image[p+1] == 0x86) {
            length = 6; target = static_cast<int64_t>(p+6)+Signed32(image+p+2);
        } else if (lea-p >= 6 && image[p] == 0xe9 && image[p+5] == 0x90) {
            length = 6; target = static_cast<int64_t>(p+5)+Signed32(image+p+1); patched = true;
        } else if (lea-p >= 2 && (image[p] == 0x76 || image[p] == 0xeb)) {
            length = 2; target = static_cast<int64_t>(p+2)+Signed8(image[p+1]); patched = image[p] == 0xeb;
        }
        if (!length || target <= static_cast<int64_t>(lea) || target > static_cast<int64_t>(lea)+0x200
            || target < code->rva || target >= static_cast<int64_t>(end)) continue;
        PatchSite site{static_cast<uint32_t>(p), {image+p, image+p+length}, {image+p, image+p+length}};
        if (length == 2) site.after[0] = 0xeb;
        else {
            const int64_t displacement = target-static_cast<int64_t>(p+5);
            if (displacement < (std::numeric_limits<int32_t>::min)() || displacement > (std::numeric_limits<int32_t>::max)()) continue;
            const auto value = static_cast<uint32_t>(displacement);
            site.after[0] = 0xe9;
            for (size_t i = 0; i < 4; ++i) site.after[i+1] = static_cast<uint8_t>(value >> (8*i));
            site.after[5] = 0x90;
        }
        candidates.emplace_back(std::move(site), patched);
    }
    if (candidates.size() != 1) return Result(PatchStatus::PatternChanged, "Expected exactly one COMISS/JBE branch with a valid forward target");
    return {candidates[0].second ? PatchStatus::AlreadyPatched : PatchStatus::Found, std::move(candidates[0].first),
        candidates[0].second ? "Threshold validation branch is already unconditional" : "Unique threshold validation branch found"};
}
}
const char* StatusName(PatchStatus status) {
    switch (status) {
    case PatchStatus::Found: return "Found";
    case PatchStatus::Patched: return "Patched";
    case PatchStatus::AlreadyPatched: return "AlreadyPatched";
    case PatchStatus::NotNeeded: return "NotNeeded";
    case PatchStatus::PatternChanged: return "PatternChanged";
    case PatchStatus::Error: return "Error";
    }
    return "Unknown";
}
FindResult FindPatchSite(const uint8_t* image, size_t imageSize) {
    try { return FindImpl(image, imageSize); }
    catch (const std::exception& e) { return {PatchStatus::Error, std::nullopt, e.what()}; }
}
FindResult ApplyPatch(uint8_t* image, size_t imageSize) {
    auto result = FindPatchSite(image, imageSize);
    if (result.status != PatchStatus::Found) return result;
#ifdef _WIN32
    auto& site = *result.site;
    auto* address = image+site.rva;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const uintptr_t pageSize = system.dwPageSize;
    const uintptr_t firstPage = reinterpret_cast<uintptr_t>(address) & ~(pageSize-1);
    const uintptr_t lastPage = reinterpret_cast<uintptr_t>(address+site.after.size()-1) & ~(pageSize-1);
    struct Page { void* address; DWORD protection; };
    std::array<Page, 2> changed{};
    size_t count = 0;
    auto restore = [&] {
        bool success = true;
        while (count) { DWORD unused = 0; const auto page = changed[--count];
            if (!VirtualProtect(page.address, pageSize, page.protection, &unused)) success = false;
        }
        return success;
    };
    for (uintptr_t page = firstPage;; page += pageSize) {
        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), pageSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            const bool restored = restore();
            result.status = PatchStatus::Error;
            result.detail = restored ? "VirtualProtect failed; no bytes written" : "VirtualProtect failed and original protection could not be restored";
            return result;
        }
        changed[count++] = {reinterpret_cast<void*>(page), oldProtection};
        if (page == lastPage) break;
    }
    const auto write = detail::GuardedWrite(address, site.before.data(), site.after.data(), site.after.size());
    if (write == detail::WriteResult::Changed) {
        const bool restored = restore();
        result.status = PatchStatus::Error;
        result.detail = restored ? "Patch bytes changed before write; no bytes written" : "Patch bytes changed and original protection could not be restored";
        return result;
    }
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, site.after.size()) != 0;
    const bool restored = restore();
    result.status = write == detail::WriteResult::Written && flushed && restored ? PatchStatus::Patched : PatchStatus::Error;
    result.detail = !restored ? "Bytes written but original protection could not be restored" :
        write == detail::WriteResult::Fault ? "Exception accessing patch bytes; page protection restored; rollback attempted if needed" :
        !flushed ? "Bytes written but FlushInstructionCache failed" : "Threshold validation branch patched in process memory";
#else
    result.status = PatchStatus::Error;
    result.detail = "Writing executable memory is supported only on Windows";
#endif
    return result;
}
}
