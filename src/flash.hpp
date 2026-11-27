#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>

namespace wave723 {

void initializeXxkg1Flash(std::ostream& flash);
void writeFlashPayload(std::ostream& flash, std::uint32_t address,
                       const std::uint8_t* data, std::size_t size);

}  // namespace wave723
