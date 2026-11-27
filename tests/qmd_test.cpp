#include "qmd.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

void word(std::vector<std::uint8_t>& data, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        data.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

}  // namespace

int main() {
    constexpr std::size_t block = 0x40000;
    constexpr std::size_t packed = 2 * block;
    constexpr std::size_t flags = 4096;
    constexpr std::size_t codeSize = 12 + flags + 16;
    std::vector<std::uint8_t> image(packed + codeSize);
    image[0] = 0x42;
    word(image, block, 0x00444d51);
    word(image, block + 8, 0x0503);
    word(image, block + 0x10, 2 * block);
    word(image, block + 0x100, 0x0f);
    word(image, block + 0x104, block);
    word(image, block + 0x10c, 1);
    word(image, block + 0x110, codeSize);
    word(image, block + 0x114, packed);
    word(image, packed, block);
    word(image, packed + 4, flags);
    std::fill_n(image.begin() + packed + 12, flags, 0x55);
    for (unsigned i = 0; i < 16; ++i) image[packed + 12 + flags + i] = i;

    const auto result = wave723::expandAppsQmd(image);
    if (result.size() != 2 * block || result[0] != 0x42 ||
        result[block + 15] != 15 || result[block + 16] != 14 ||
        result[2 * block - 1] != 15) {
        std::cerr << "Incorrect QMD output\n";
        return 1;
    }
    image.pop_back();
    try {
        wave723::expandAppsQmd(image);
        std::cerr << "Truncated QMD stream accepted\n";
        return 1;
    } catch (const std::runtime_error&) {
        return 0;
    }
}
