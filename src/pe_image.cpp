#include "pe_image.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hlaftp {
namespace {
void CheckRange(size_t offset, size_t count, size_t total, const char* message) {
    if (offset > total || count > total - offset) throw std::runtime_error(message);
}
uint16_t U16(std::span<const uint8_t> bytes, size_t offset) {
    CheckRange(offset, 2, bytes.size(), "Truncated PE field");
    return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset+1]) << 8));
}
uint32_t U32(std::span<const uint8_t> bytes, size_t offset) {
    CheckRange(offset, 4, bytes.size(), "Truncated PE field");
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[offset+i]) << (8*i);
    return value;
}
struct SectionFile { uint32_t rawOffset, rawSize; };
struct Headers { PeImage image; uint32_t sizeOfHeaders; std::vector<SectionFile> files; };
bool Overlaps(uint32_t start, uint32_t size, uint32_t other, uint32_t otherSize) {
    return size && otherSize && static_cast<uint64_t>(start) < static_cast<uint64_t>(other)+otherSize
        && static_cast<uint64_t>(other) < static_cast<uint64_t>(start)+size;
}
Headers ParseHeaders(std::span<const uint8_t> bytes, bool mapped) {
    CheckRange(0, 64, bytes.size(), "Truncated DOS header");
    if (U16(bytes, 0) != 0x5a4d) throw std::runtime_error("Invalid DOS signature");
    const size_t nt = U32(bytes, 0x3c);
    if (nt < 64) throw std::runtime_error("NT header overlaps DOS header");
    CheckRange(nt, 24, bytes.size(), "Truncated NT header");
    if (U32(bytes, nt) != 0x4550 || U16(bytes, nt+4) != 0x8664)
        throw std::runtime_error("Expected AMD64 PE image");
    const size_t count = U16(bytes, nt+6);
    if (!count || count > 96) throw std::runtime_error("Invalid PE section count");
    const size_t optionalSize = U16(bytes, nt+20);
    const size_t optional = nt+24;
    CheckRange(optional, optionalSize, bytes.size(), "Truncated optional header");
    if (optionalSize < 112 || U16(bytes, optional) != 0x20b)
        throw std::runtime_error("Expected PE32+ optional header");
    const size_t table = optional+optionalSize;
    CheckRange(table, count*40, bytes.size(), "Truncated PE section table");
    Headers parsed{{U32(bytes, nt+8), U32(bytes, optional+56), {}}, U32(bytes, optional+60), {}};
    if (!parsed.image.sizeOfImage || parsed.sizeOfHeaders < table+count*40
        || parsed.sizeOfHeaders > parsed.image.sizeOfImage)
        throw std::runtime_error("Invalid PE header/image size");
    CheckRange(0, parsed.sizeOfHeaders, bytes.size(), "Truncated PE headers");
    if (mapped) CheckRange(0, parsed.image.sizeOfImage, bytes.size(), "Truncated mapped image");
    parsed.image.sections.reserve(count);
    parsed.files.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t row = table+i*40;
        const uint32_t rawSize = U32(bytes, row+16);
        const uint32_t size = (std::max)(U32(bytes, row+8), rawSize);
        const uint32_t rva = U32(bytes, row+12);
        CheckRange(rva, size, parsed.image.sizeOfImage, "PE section exceeds image");
        if (size && rva < parsed.sizeOfHeaders) throw std::runtime_error("PE section overlaps headers");
        for (const auto& existing : parsed.image.sections)
            if (Overlaps(rva, size, existing.rva, existing.size)) throw std::runtime_error("Overlapping PE sections");
        const uint32_t rawOffset = U32(bytes, row+20);
        if (!mapped && rawSize) {
            CheckRange(rawOffset, rawSize, bytes.size(), "Truncated PE section data");
            if (rawOffset < parsed.sizeOfHeaders) throw std::runtime_error("PE raw section overlaps headers");
            for (const auto& existing : parsed.files)
                if (Overlaps(rawOffset, rawSize, existing.rawOffset, existing.rawSize))
                    throw std::runtime_error("Overlapping PE raw sections");
        }
        parsed.image.sections.push_back({rva, size, (U32(bytes, row+36) & 0x20000000u) != 0});
        parsed.files.push_back({rawOffset, rawSize});
    }
    return parsed;
}
}
PeImage ParseMappedImage(std::span<const uint8_t> bytes) { return ParseHeaders(bytes, true).image; }
std::vector<uint8_t> MapFileImage(std::span<const uint8_t> bytes) {
    const auto parsed = ParseHeaders(bytes, false);
    // Runtime modules are small; reject unreasonable allocations from an untrusted header.
    if (parsed.image.sizeOfImage > 512u*1024u*1024u) throw std::runtime_error("PE image exceeds 512 MiB mapping limit");
    std::vector<uint8_t> image(parsed.image.sizeOfImage, 0);
    std::memcpy(image.data(), bytes.data(), parsed.sizeOfHeaders);
    for (size_t i = 0; i < parsed.files.size(); ++i) {
        const auto file = parsed.files[i];
        if (file.rawSize) std::memcpy(image.data()+parsed.image.sections[i].rva, bytes.data()+file.rawOffset, file.rawSize);
    }
    return image;
}
}
