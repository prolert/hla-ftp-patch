#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace hlaftp {
struct PeSection { uint32_t rva; uint32_t size; bool executable; };
struct PeImage {
    uint32_t timestamp;
    uint32_t sizeOfImage;
    std::vector<PeSection> sections;
};
PeImage ParseMappedImage(std::span<const uint8_t> bytes);
std::vector<uint8_t> MapFileImage(std::span<const uint8_t> bytes);
}
