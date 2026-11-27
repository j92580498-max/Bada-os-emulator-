#include "qmd.hpp"
#include "machine.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace wave723 {
namespace {

constexpr std::size_t blockSize = 256 * 1024;
constexpr std::size_t pageSize = 4096;
constexpr std::size_t maxExpandedSize = 128 * 1024 * 1024;
constexpr std::size_t pagedBlock = maxExpandedSize;
constexpr std::uint32_t qmdMagic = 0x00444d51;

// QMD 0x0503 code dictionary documented by libSFormat.
constexpr std::array<std::uint16_t, 256> codeDeltas = {
    0x0001, 0x0003, 0x0100, 0x0002, 0x0008, 0x0007, 0x0006, 0x0300,
    0x0010, 0x0004, 0x0200, 0x0009, 0x0040, 0x0018, 0x0005, 0x0020,
    0x000c, 0x000e, 0x000f, 0x000a, 0x00c0, 0x0800, 0x0700, 0x0101,
    0x0400, 0x000b, 0x0030, 0x0011, 0x0080, 0x0600, 0x000d, 0x0012,
    0x001c, 0x0500, 0x001b, 0x001e, 0x0014, 0x001a, 0x0028, 0x0038,
    0x1000, 0x001f, 0x0019, 0x0016, 0x0060, 0x2000, 0x0013, 0x001d,
    0x0103, 0x0024, 0x0017, 0x0015, 0x0102, 0x01c0, 0x0f00, 0x003c,
    0x0301, 0x0c00, 0x1800, 0x0048, 0x0021, 0x0034, 0x0e00, 0x0202,
    0x002c, 0x0070, 0x0a00, 0x0303, 0x0036, 0x0201, 0x003f, 0x0d00,
    0x0180, 0x003e, 0x3000, 0x0900, 0x0078, 0x0022, 0x0050, 0x003a,
    0x0041, 0x0107, 0x0033, 0x0106, 0x0026, 0x002a, 0x00a0, 0x0023,
    0x0029, 0x0088, 0x0044, 0x003d, 0x00e0, 0x0032, 0x002e, 0x0039,
    0x0031, 0x002d, 0x00f0, 0x0140, 0x0b00, 0x003b, 0x0058, 0x4000,
    0x0037, 0x0035, 0x0068, 0x0302, 0x007c, 0x002f, 0x0027, 0x0064,
    0x0090, 0x0074, 0x0203, 0x0104, 0x006c, 0x1100, 0x03c0, 0x00ff,
    0x0025, 0xf000, 0x1f00, 0x0701, 0x0042, 0x007f, 0x002b, 0x0105,
    0x0054, 0x1c00, 0x004c, 0x0801, 0x0043, 0x6000, 0x005c, 0x007e,
    0x00e8, 0x0108, 0x00f8, 0xe000, 0x0206, 0x1e00, 0x0380, 0x0061,
    0x007a, 0x004e, 0x0601, 0x1001, 0x00c8, 0x8000, 0x1d00, 0x00d0,
    0x0072, 0x0049, 0x1600, 0x1a00, 0x0046, 0x7000, 0x010f, 0x0110,
    0x0076, 0x1200, 0x1400, 0x0404, 0x0606, 0x010e, 0x00fc, 0x1700,
    0x006e, 0x00fe, 0x1300, 0x0062, 0x0066, 0xc000, 0x0204, 0x0306,
    0x0063, 0x0707, 0x0280, 0x0602, 0x0055, 0x0047, 0x006a, 0x010c,
    0x0052, 0x0501, 0x00d8, 0x0307, 0x0073, 0x0109, 0x0808, 0x0401,
    0x004a, 0x2020, 0x005a, 0x0702, 0x00b0, 0x0045, 0x0207, 0x0304,
    0x0402, 0x005e, 0x010a, 0x0079, 0x3800, 0x00f4, 0x1500, 0x01e0,
    0x1b00, 0x0071, 0x1010, 0x00c1, 0x00e4, 0x0502, 0x0056, 0x007d,
    0x0081, 0x0077, 0x00cc, 0x0703, 0x010d, 0x0205, 0x0340, 0x5000,
    0x0082, 0x0067, 0xff00, 0x0120, 0x0069, 0x0098, 0x00c3, 0x1900,
    0x0065, 0x007b, 0x0240, 0x0603, 0x00ec, 0x0059, 0x00fa, 0x0403,
    0x0075, 0x006f, 0x3100, 0x3300, 0x004f, 0x00b8, 0x006d, 0x0208,
    0x004d, 0x0111, 0x0051, 0x020e, 0x00dc, 0x00c4, 0x2100, 0x00a8,
};

[[noreturn]] void invalidQmd() {
    throw std::runtime_error("Malformed APPS QMD image.");
}

struct Bytes {
    const std::uint8_t* data;
    std::size_t size;

    Bytes sub(std::size_t offset, std::size_t length) const {
        if (offset > size || length > size - offset) invalidQmd();
        return {data + offset, length};
    }

    std::uint16_t u16(std::size_t position) const {
        const auto* p = sub(position, 2).data;
        return p[0] | (std::uint16_t(p[1]) << 8);
    }

    std::uint32_t u32(std::size_t position) const {
        return u16(position) | (std::uint32_t(u16(position + 2)) << 16);
    }
};

struct Cursor {
    Bytes source;
    std::size_t position = 0;

    Bytes take(std::size_t count) {
        const auto result = source.sub(position, count);
        position += count;
        return result;
    }

    std::uint8_t byte() { return *take(1).data; }
    std::uint16_t word() { return take(2).u16(0); }
};

class Flags {
public:
    explicit Flags(Bytes source) : bytes_{source} {}

    bool bit() {
        if (!remaining_) {
            current_ = bytes_.byte();
            remaining_ = 8;
        }
        --remaining_;
        return (current_ >> remaining_) & 1;
    }

private:
    Cursor bytes_;
    std::uint8_t current_ = 0;
    unsigned remaining_ = 0;
};

void decodeCode(Bytes input, std::uint8_t* output, std::size_t outputSize) {
    if (input.u32(0) != outputSize) invalidQmd();
    Cursor bytes{input};
    bytes.take(12);
    Flags flags(bytes.take(input.u32(4)));
    Cursor symbols{bytes.take(input.u32(8))};
    std::size_t written = 0;
    std::size_t distance = 2;

    const auto literal = [&](std::size_t count) {
        const auto value = bytes.take(count);
        std::copy_n(value.data, count, output + written);
        written += count;
    };
    const auto reference = [&](std::size_t count, std::uint16_t delta = 0) {
        if (!distance || distance > written) invalidQmd();
        for (std::size_t i = 0; i < count; ++i) {
            output[written] = output[written - distance] ^ (delta >> (8 * (i & 1)));
            ++written;
        }
    };

    literal(16);
    while (written < outputSize) {
        if (!flags.bit()) {
            if (flags.bit()) reference(16);
            else literal(16);
            continue;
        }
        const auto reuse = symbols.byte();
        for (unsigned i = 0; i < 8; ++i) {
            if (i % 2 == 0 && !flags.bit()) {
                distance = 2 * (flags.bit() ? symbols.byte() : bytes.word());
            }
            if (reuse & (0x80 >> i)) {
                reference(2);
            } else if (flags.bit()) {
                literal(2);
            } else {
                reference(2, codeDeltas[symbols.byte()]);
            }
        }
    }
}

void decodePages(Bytes input, std::uint8_t* output) {
    constexpr std::size_t pages = blockSize / pageSize;
    Cursor encoded{input};
    const auto sizes = encoded.take(pages * 2);
    for (std::size_t page = 0; page < pages; ++page) {
        const auto data = encoded.take(sizes.u16(2 * page));
        if (data.size == pageSize) {
            std::copy_n(data.data, pageSize, output);
        } else {
            decodeCode(data, output, pageSize);
        }
        output += pageSize;
    }
}

std::size_t layoutAppsBlocks(Bytes header, std::vector<std::size_t>& offsets) {
    constexpr std::size_t maxSections = 8;
    constexpr std::uint32_t resident = 1;
    constexpr std::uint32_t paged = 8;
    const auto count = header.u32(0x0c);
    if (count == 0) {
        for (std::size_t i = 0; i < offsets.size(); ++i) offsets[i] = i * blockSize;
        return offsets.size() * blockSize;
    }
    if (count > maxSections) invalidQmd();

    // The first raw block holds the reset vectors and precedes the section table.
    std::size_t next = 1;
    std::size_t size = blockSize;
    std::array<bool, maxExpandedSize / blockSize> occupied{};
    occupied[0] = true;
    offsets[0] = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto section = header.sub(0x80 + 16 * i, 16);
        const auto type = section.u32(0);
        const auto source = section.u32(4);
        const auto address = section.u32(8);
        const auto length = section.u32(12);
        const auto blocks = length / blockSize;
        if (source != next * blockSize || length == 0 || length % blockSize ||
            blocks > offsets.size() - next || address % blockSize ||
            std::uint64_t(address) + length > (std::uint64_t{1} << 32)) {
            invalidQmd();
        }
        if (type == paged) {
            // The firmware uses a virtual range for this section after activating paging.
            std::fill_n(offsets.begin() + next, blocks, pagedBlock);
        } else if (type == resident) {
            if (address < appsBase) invalidQmd();
            const std::size_t start = address - appsBase;
            if (start >= maxExpandedSize || length > maxExpandedSize - start) invalidQmd();
            for (std::size_t j = 0; j < blocks; ++j) {
                const auto block = start / blockSize + j;
                if (occupied[block]) invalidQmd();
                occupied[block] = true;
                offsets[next + j] = block * blockSize;
            }
            size = std::max(size, start + length);
        } else {
            invalidQmd();
        }
        next += blocks;
    }
    if (next != offsets.size()) invalidQmd();
    return size;
}

}  // namespace

std::vector<std::uint8_t> expandAppsQmd(std::vector<std::uint8_t> image) {
    const Bytes input{image.data(), image.size()};
    constexpr std::array<std::size_t, 2> headerOffsets{0x40000, 0x80000};
    const auto found = std::find_if(headerOffsets.begin(), headerOffsets.end(),
        [&](std::size_t offset) {
            return input.size >= offset + 4 && input.u32(offset) == qmdMagic;
        });
    if (found == headerOffsets.end()) return image;

    const auto tableStart = *found + 0x100;
    if (input.u16(*found + 8) != 0x0503) {
        throw std::runtime_error("Unsupported APPS QMD compression version.");
    }
    const auto expandedSize = input.u32(*found + 0x10);
    if (expandedSize == 0 || expandedSize > maxExpandedSize || expandedSize % blockSize) {
        invalidQmd();
    }
    const auto entries = expandedSize / blockSize;
    const auto table = input.sub(tableStart, 12 * (entries + 1));
    if (table.u32(12 * entries) != 0) invalidQmd();

    std::vector<std::size_t> offsets(entries);
    std::vector<std::uint8_t> expanded(layoutAppsBlocks(input.sub(*found, 0x100), offsets));
    for (std::size_t index = 0; index < entries; ++index) {
        try {
            const auto row = table.sub(12 * index, 12);
            const auto block = input.sub(row.u32(8), row.u32(4));
            if (offsets[index] == pagedBlock) continue;
            auto* output = expanded.data() + offsets[index];
            switch (row.u32(0)) {
            case 0x0f:
            case 0x0a:
            case 0x03:
                if (block.size != blockSize) invalidQmd();
                std::copy_n(block.data, blockSize, output);
                break;
            case 0x01:
                decodeCode(block, output, blockSize);
                break;
            case 0x08:
                decodePages(block, output);
                break;
            default:
                invalidQmd();
            }
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("APPS QMD entry " + std::to_string(index) + ": " + error.what());
        }
    }
    return expanded;
}

}  // namespace wave723
