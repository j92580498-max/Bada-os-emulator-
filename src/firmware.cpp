#include "wave723/firmware.hpp"

#include "machine.hpp"
#include "qmd.hpp"
#include "seed.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace wave723 {
namespace {

constexpr std::uint64_t maxZipBytes = 512ULL * 1024 * 1024;
constexpr std::uint32_t armResetLoad = 0xe59ff024;
constexpr std::uint32_t amssTag = 0xbabeface;
constexpr std::size_t resetLiteral = 0x2c;
constexpr std::size_t probeHeaderSize = 64;
constexpr std::uint32_t appsResetLoad = 0xe59ff468;
constexpr std::size_t appsResetLiteral = 0x470;
constexpr std::size_t flashFooterSize = 1024;
constexpr std::uint64_t maxAppsSize = 128 * 1024 * 1024;
constexpr std::uint32_t footerMagic = 0xabcdabcd;
constexpr std::uint32_t seedMagic = 0x79461379;

std::uint32_t word(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

std::string_view basename(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    return slash == name.npos ? name : name.substr(slash + 1);
}

std::string lowerAscii(std::string_view name) {
    std::string lower(name);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return lower;
}

bool endsWith(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

std::string_view role(std::string_view name) {
    const auto lower = lowerAscii(basename(name));
    if (lower == "amss.bin") return "AMSS";
    if (lower == "apps_compressed.bin") return "APPS";
    if (endsWith(lower, ".rc1")) return "RSRC1";
    if (endsWith(lower, ".rc2")) return "RSRC2";
    if (endsWith(lower, ".csc")) return "CSC";
    if (endsWith(lower, ".app")) return "SHPAPP";
    if (endsWith(lower, ".pfs")) return "PFS";
    if (endsWith(lower, ".fota")) return "FOTA";
    if (lower == "boot_loader.mbn" || lower == "dbl.mbn" || lower == "boot.bin") {
        return "BOOT";
    }
    return "OTHER";
}

class Archive {
public:
    explicit Archive(const std::filesystem::path& path)
        : file_(path, std::ios::binary | std::ios::ate) {
        const auto length = file_.tellg();
        if (length < 0) throw std::runtime_error("Could not open the firmware ZIP.");
        if (static_cast<std::uint64_t>(length) > maxZipBytes) {
            throw std::runtime_error("Firmware ZIP exceeds the 512 MiB limit.");
        }
        zip_.m_pIO_opaque = &file_;
        zip_.m_pRead = [](void* context, mz_uint64 offset, void* dest, size_t size) -> size_t {
            auto& file = *static_cast<std::ifstream*>(context);
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(static_cast<char*>(dest), static_cast<std::streamsize>(size));
            return static_cast<size_t>(file.gcount());
        };
        if (!mz_zip_reader_init(&zip_, static_cast<mz_uint64>(length), 0)) {
            fail("Could not read the firmware ZIP");
        }
    }

    ~Archive() { mz_zip_reader_end(&zip_); }
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    mz_uint count() { return mz_zip_reader_get_num_files(&zip_); }

    mz_zip_archive_file_stat stat(mz_uint index) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&zip_, index, &info)) fail("Could not read ZIP entry");
        return info;
    }

    std::vector<std::uint8_t> header(mz_uint index) {
        auto* iter = mz_zip_reader_extract_iter_new(&zip_, index, 0);
        if (!iter) fail("Could not open ZIP entry");
        std::vector<std::uint8_t> bytes(probeHeaderSize);
        bytes.resize(mz_zip_reader_extract_iter_read(iter, bytes.data(), bytes.size()));
        mz_zip_reader_extract_iter_free(iter);
        return bytes;
    }

    std::vector<std::uint8_t> image(const mz_zip_archive_file_stat& info) {
        constexpr auto maxAmssSize = probeRamSize - (amssBase - probeRamBase);
        if (info.m_uncomp_size < resetLiteral + 4 || info.m_uncomp_size > maxAmssSize) {
            throw std::runtime_error("The AMSS image size is outside the reset probe's RAM range.");
        }
        return extract(info, "Could not decompress and verify amss.bin");
    }

    mz_zip_archive_file_stat find(std::string_view part, const std::string& name) {
        mz_zip_archive_file_stat result{};
        bool found = false;
        const auto entries = count();
        for (mz_uint index = 0; index < entries; ++index) {
            const auto entry = stat(index);
            if (entry.m_is_directory || role(entry.m_filename) != part) continue;
            if (found) throw std::runtime_error("The ZIP contains multiple " + name + " files.");
            result = entry;
            found = true;
        }
        if (!found) throw std::runtime_error("The ZIP does not contain " + name + ".");
        return result;
    }

    std::vector<std::uint8_t> bootLogo() {
        constexpr std::size_t table = 0x40000;
        constexpr std::size_t size = PanelFrame::width * PanelFrame::height * 2;
        const auto entry = find("RSRC2", "RC2 resource");
        if (entry.m_uncomp_size < table + 16 + size || entry.m_uncomp_size > 16 * 1024 * 1024) {
            throw std::runtime_error("The RC2 resource is outside the supported size range.");
        }
        const auto image = extract(entry, "Could not decompress and verify the RC2 resource");
        const auto* fields = image.data() + table;
        const auto offset = word(fields + 8);
        if (word(fields) != 0x5a5b5c5d || word(fields + 4) != table ||
            word(fields + 12) != size || offset < table + 16 || offset > image.size() - size) {
            throw std::runtime_error("The RC2 resource has no supported 240x400 startup bitmap.");
        }
        return {image.begin() + offset, image.begin() + offset + size};
    }

    std::vector<std::uint8_t> appsResetImage(const mz_zip_archive_file_stat& info) {
        if (info.m_uncomp_size < flashFooterSize + appsResetLiteral + 4 ||
            info.m_uncomp_size > maxAppsSize) {
            throw std::runtime_error("The APPS image size is outside the supported range.");
        }
        const auto payloadSize = static_cast<std::size_t>(info.m_uncomp_size) - flashFooterSize;
        if (payloadSize % 16 != 0) {
            throw std::runtime_error("The APPS payload must contain complete SEED blocks.");
        }
        auto bytes = extract(info, "Could not decompress and verify apps_compressed.bin");
        const auto* footer = bytes.data() + payloadSize;
        if (word(footer) != footerMagic || word(footer + 0x48) != seedMagic ||
            word(footer + 0x190) != seedMagic || word(footer + 0x194) != payloadSize) {
            throw std::runtime_error("Unsupported APPS footer. Expected the XXKG1 SEED layout.");
        }
        std::array<std::uint32_t, 32> keys{};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i] = word(footer + 0x110 + 4 * i);
        }
        decryptSamsungSeed(bytes.data(), payloadSize / 16, keys);
        bytes.resize(payloadSize);
        return expandAppsQmd(std::move(bytes));
    }

private:
    std::vector<std::uint8_t> extract(const mz_zip_archive_file_stat& info,
                                      const char* operation) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip_, info.m_file_index, bytes.data(), bytes.size(), 0)) {
            fail(operation);
        }
        return bytes;
    }

    [[noreturn]] void fail(const char* operation) {
        throw std::runtime_error(std::string(operation) + ": " +
            mz_zip_get_error_string(mz_zip_get_last_error(&zip_)) + ".");
    }

    std::ifstream file_;
    mz_zip_archive zip_{};
};

std::string_view signature(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4) return "unknown";
    if (word(bytes.data()) == armResetLoad && bytes.size() >= 0x24 &&
        word(bytes.data() + 0x20) == amssTag) return "ARM reset vector";
    if (word(bytes.data()) == 0x00444d51) return "QMD";
    if (word(bytes.data()) == 0xcafebabe) return "CSC header";
    if (word(bytes.data()) == 0xe12fff1f) return "ARM branch entry";
    if (bytes.size() >= 12) {
        constexpr std::array<std::uint8_t, 12> fim{'F', 0, 'i', 0, 'm', 0, 'B', 0, 'I', 0, 'N', 0};
        constexpr std::array<std::uint8_t, 12> pfs{'P', 0, 'F', 0, 'S', 0, 'B', 0, 'I', 0, 'N', 0};
        if (std::equal(fim.begin(), fim.end(), bytes.begin())) return "FimBIN";
        if (std::equal(pfs.begin(), pfs.end(), bytes.begin())) return "PFSBIN";
    }
    return "unknown";
}

void writeName(std::ostream& out, std::string_view name) {
    for (unsigned char ch : name) {
        out << static_cast<char>(ch >= 32 && ch <= 126 ? ch : '?');
    }
}

std::filesystem::path temporaryDirectory(const std::filesystem::path& path) {
#ifdef __ANDROID__
    return path.parent_path();
#else
    return std::filesystem::temp_directory_path();
#endif
}

std::string traceImage(const std::filesystem::path& path, ResetTarget target,
                       std::uint32_t instructionLimit, const std::filesystem::path& qemu,
                       bool diagnostic) {
    if (instructionLimit == 0 || instructionLimit > maxInstructionLimit) {
        throw std::runtime_error("Instruction limit must be between 1 and 2000000.");
    }
    const bool apps = target == ResetTarget::Apps;
    const std::string_view part = apps ? "APPS" : "AMSS";
    const std::string filename = apps ? "apps_compressed.bin" : "amss.bin";
    Archive archive(path);
    const auto entry = archive.find(part, filename);
    const auto bytes = apps ? archive.appsResetImage(entry) : archive.image(entry);
    if (apps && (word(bytes.data()) != appsResetLoad || word(bytes.data() + 4) != appsResetLoad)) {
        throw std::runtime_error("Unsupported APPS reset header. Expected the XXKG1 vector layout.");
    }
    if (!apps && (word(bytes.data()) != armResetLoad || word(bytes.data() + 0x20) != amssTag)) {
        throw std::runtime_error("Unsupported AMSS reset header. Expected the XXKG1 vector layout.");
    }
    const auto handler = word(bytes.data() + (apps ? appsResetLiteral : resetLiteral));
    const auto base = apps ? appsBase : amssBase;
    if ((handler & 3) || handler < base || handler - base >= bytes.size() - 3) {
        throw std::runtime_error(std::string(part) + " reset handler points outside the staged image.");
    }
    return runReset(target, bytes, handler, instructionLimit, qemu, temporaryDirectory(path),
                    diagnostic);
}

}  // namespace

PanelFrame renderBootLogo(const std::filesystem::path& path, const std::filesystem::path& qemu) {
    Archive archive(path);
    const auto bitmap = archive.bootLogo();
    constexpr auto pixels = PanelFrame::width * PanelFrame::height;
    constexpr std::array<std::uint32_t, 20> program{
        0xe59f0040,                   // ldr r0, LCD controller address
        0xe28f1044,                   // adr r1, first RGB565 pixel
        0xe59f203c,                   // ldr r2, pixel count
        0xe3a03011, 0xe5803000,       // Sleep out
        0xe3a0303a, 0xe5803000,       // Set pixel format
        0xe3a03055, 0xe5803004,       // RGB565
        0xe3a0302c, 0xe5803000,       // Begin GRAM write
        0xe0d130b2,                   // loop: ldrh r3, [r1], #2
        0xe5803004,                   // str r3, [r0, #4]
        0xe2522001, 0x1afffffb,       // subs r2, r2, #1; bne loop
        0xe3a03029, 0xe5803000,       // Display on
        0xeafffffe,                   // b .
        0x08030000, pixels,
    };
    std::vector<std::uint8_t> image;
    image.reserve(program.size() * 4 + bitmap.size());
    for (const auto opcode : program) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            image.push_back(static_cast<std::uint8_t>(opcode >> shift));
        }
    }
    image.insert(image.end(), bitmap.begin(), bitmap.end());
    PanelFrame frame;
    runReset(ResetTarget::Amss, image, amssBase, 4 * pixels + 32, qemu,
             temporaryDirectory(path), false, &frame);
    return frame;
}

void savePanelPng(const PanelFrame& frame, const std::filesystem::path& path) {
    constexpr int channels = 3;
    if (frame.rgb.size() != PanelFrame::width * PanelFrame::height * channels) {
        throw std::runtime_error("Invalid LCD frame size.");
    }
    std::size_t size = 0;
    std::unique_ptr<void, decltype(&mz_free)> png(
        tdefl_write_image_to_png_file_in_memory(frame.rgb.data(), PanelFrame::width,
                                               PanelFrame::height, channels, &size),
        mz_free);
    if (!png) throw std::runtime_error("Could not encode the LCD capture as PNG.");
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char*>(png.get()), size);
    output.close();
    if (!output) throw std::runtime_error("Could not save the LCD capture.");
}

std::string inspectFirmware(const std::filesystem::path& path) {
    Archive archive(path);
    std::ostringstream out;
    out << "Samsung Wave 723 / BCM2156\n\n";
    bool hasBoot = false;
    for (mz_uint index = 0; index < archive.count(); ++index) {
        const auto entry = archive.stat(index);
        if (entry.m_is_directory) continue;
        const auto part = role(entry.m_filename);
        hasBoot |= part == "BOOT";
        out << part << "  ";
        writeName(out, entry.m_filename);
        out << "\n  " << entry.m_uncomp_size << " bytes, CRC32 "
            << std::hex << std::setfill('0') << std::setw(8) << entry.m_crc32 << std::dec;
        if (part != "OTHER" && entry.m_is_supported) {
            out << ", " << signature(archive.header(index));
        }
        if (!entry.m_is_supported) out << ", unsupported ZIP member";
        out << '\n';
    }
    if (!hasBoot) out << "\nNo recognized standalone bootloader in this ZIP.\n";
    return out.str();
}

std::string traceReset(const std::filesystem::path& path, std::uint32_t instructionLimit,
                       const std::filesystem::path& qemu, bool diagnostic) {
    return traceImage(path, ResetTarget::Amss, instructionLimit, qemu, diagnostic);
}

std::string traceAppsReset(const std::filesystem::path& path, std::uint32_t instructionLimit,
                           const std::filesystem::path& qemu, bool diagnostic) {
    return traceImage(path, ResetTarget::Apps, instructionLimit, qemu, diagnostic);
}

}  // namespace wave723
