#include "flash.hpp"
#include "wave723_nand_geometry.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace wave723 {
namespace {

constexpr std::size_t pageBytes = WAVE723_NAND_PAGE_SIZE;
constexpr std::size_t sectorBytes = 512;
constexpr std::size_t pagesPerBlock = WAVE723_NAND_PAGES_PER_BLOCK;
constexpr std::uint16_t reservoir = 0x7d4;
using Record = std::array<std::uint8_t, WAVE723_NAND_RECORD_SIZE>;

void put16(std::uint8_t* dest, std::uint16_t value) {
    dest[0] = static_cast<std::uint8_t>(value);
    dest[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t* dest, std::uint32_t value) {
    put16(dest, static_cast<std::uint16_t>(value));
    put16(dest + 2, static_cast<std::uint16_t>(value >> 16));
}

void partitionInfo(std::uint8_t* sector) {
    // Stock XXKG1 builds this FSRPartI in RAM before opening the volume.
    struct Partition { std::uint16_t id, attrs, start, count; };
    constexpr std::array<Partition, 8> parts{{
        {0x09, 0x1002, 0x000, 0x001}, {0x00, 0x1002, 0x001, 0x007},
        {0x01, 0x1001, 0x008, 0x22c}, {0x14, 0x1101, 0x234, 0x4c4},
        {0x15, 0x1101, 0x6f8, 0x0b4}, {0x17, 0x1001, 0x7ac, 0x010},
        {0x18, 0x1001, 0x7bc, 0x008}, {0x19, 0x1001, 0x7c4, 0x010},
    }};
    std::fill_n(sector, sectorBytes, 0);
    std::memcpy(sector, "FSRPARTI", 8);
    put32(sector + 8, 0x20100106);
    put32(sector + 12, parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto* entry = sector + 16 + 16 * i;
        put16(entry, parts[i].id);
        put16(entry + 2, parts[i].attrs);
        put16(entry + 4, parts[i].start);
        put16(entry + 6, parts[i].count);
    }
}

void writeControlBlock(std::ostream& flash, bool locked) {
    const auto block = locked ? reservoir : reservoir + 1;
    flash.seekp(std::streamoff(block) * pagesPerBlock * WAVE723_NAND_RECORD_SIZE);
    for (unsigned page = 0; page < 4; ++page) {
        Record record;
        record.fill(0xff);
        auto* main = record.data();
        if (page == 0) {
            std::memcpy(main, locked ? "LPCH" : "UPCH", 4);
            put16(main + 4, reservoir + 3);  // Refresh block
            put16(main + 6, reservoir + 2);  // Temporary control block
            put16(main + 8, 0);              // MLC reservoir blocks
            put32(main + 12, 1);             // Local age
            put32(main + 16, locked ? 1 : 2);
            put32(main + 20, 1);             // Planes
            put32(main + 24, 1);             // Dies
            put32(main + 28, WAVE723_NAND_BLOCKS);
            put32(main + 32, WAVE723_NAND_BLOCKS);
            if (locked) {
                partitionInfo(main + sectorBytes);
            } else {
                put32(main + sectorBytes + 4, 0);  // Partition extension length
                put16(main + 2 * sectorBytes, 0);  // Erase refresh list length
                auto* allocation = main + 3 * sectorBytes;
                std::fill_n(allocation, sectorBytes, 0);
                allocation[0] = 0xf0;  // LPCB, UPCB, TPCB and REF
            }
        } else if (page == 3) {
            std::fill_n(main, pageBytes, 0);  // FSR metadata confirmation
        }
        put16(main + pageBytes + 2, 0xa5a5);  // Spare bad mark stays 0xffff
        for (auto& byte : record) byte = static_cast<std::uint8_t>(~byte);
        flash.write(reinterpret_cast<const char*>(main), record.size());
    }
}

}  // namespace

void initializeXxkg1Flash(std::ostream& flash) {
    constexpr std::streamoff size = std::streamoff(WAVE723_NAND_PAGES) * WAVE723_NAND_RECORD_SIZE;
    flash.seekp(size - 1);
    flash.put(0);
    writeControlBlock(flash, true);
    writeControlBlock(flash, false);
}

void writeFlashPayload(std::ostream& flash, std::uint32_t address,
                       const std::uint8_t* data, std::size_t size) {
    constexpr std::uint32_t end = reservoir * pagesPerBlock * pageBytes;
    if (address % pageBytes || address >= end || size > end - address) {
        throw std::runtime_error("Invalid Wave 723 flash payload extent.");
    }
    flash.seekp(std::streamoff(address / pageBytes) * WAVE723_NAND_RECORD_SIZE);
    while (size) {
        const auto count = std::min(size, pageBytes);
        Record record{};  // Implicit erased tail and spare area
        for (std::size_t i = 0; i < count; ++i) record[i] = static_cast<std::uint8_t>(~data[i]);
        flash.write(reinterpret_cast<const char*>(record.data()), record.size());
        data += count;
        size -= count;
    }
}

}  // namespace wave723
