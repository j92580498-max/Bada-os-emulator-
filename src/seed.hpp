#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wave723 {

void decryptSamsungSeed(std::uint8_t* data, std::size_t blockCount,
                        std::array<std::uint32_t, 32> roundKeys);

}  // namespace wave723
