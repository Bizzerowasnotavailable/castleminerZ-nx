#include "cmz/game_image.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace cmz {
namespace {

struct RequiredFile {
    const char* path;
    std::uint32_t expected_size;
};

constexpr std::array<RequiredFile, 6> kRequiredFiles{{
    {"584E07D1/CastleMinerZ.exe", 439296},
    {"584E07D1/DNA.Common.dll", 610304},
    {"584E07D1/Content/LoadScreen.xnb", 108468},
    {"584E07D1/Content/Sounds.xwb", 10247444},
    {"584E07D1/Content/Terrain/Textures.xnb", 2468858},
    {"GameInfo.xml", 2422},
}};

std::uint16_t read_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool is_il_only_anycpu_pe(const StfsPackage& package, const StfsEntry& entry,
                          std::string& error) {
    constexpr std::uint32_t kComImageFlagsIlOnly = 0x00000001;
    constexpr std::uint32_t kComImageFlags32BitRequired = 0x00000002;
    constexpr std::uint32_t kComImageFlagsNativeEntryPoint = 0x00000010;

    std::array<std::uint8_t, 4096> header{};
    const std::size_t amount = entry.size < header.size() ? entry.size : header.size();
    if (amount < 256 || !package.read_range(entry, 0, header.data(), amount, error)) {
        return false;
    }
    if (header[0] != 'M' || header[1] != 'Z') {
        return false;
    }

    const std::uint32_t pe_offset = read_le32(header.data() + 0x3C);
    if (pe_offset > amount - 24 ||
        std::memcmp(header.data() + pe_offset, "PE\0\0", 4) != 0) {
        return false;
    }

    const std::size_t optional = pe_offset + 24;
    const std::uint16_t optional_size = read_le16(header.data() + pe_offset + 20);
    if (optional_size < 2 || optional > amount || optional_size > amount - optional) {
        return false;
    }
    const std::uint16_t optional_magic = read_le16(header.data() + optional);
    std::size_t directory_offset = 0;
    std::size_t directory_count_offset = 0;
    if (optional_magic == 0x10B) {
        directory_offset = 96;
        directory_count_offset = 92;
    } else if (optional_magic == 0x20B) {
        directory_offset = 112;
        directory_count_offset = 108;
    } else {
        return false;
    }
    if (optional_size < directory_offset + 15 * 8 ||
        read_le32(header.data() + optional + directory_count_offset) < 15) {
        return false;
    }

    const std::size_t directory_base = optional + directory_offset;
    const std::size_t cli_directory = directory_base + 14 * 8;
    const std::uint32_t cli_rva = read_le32(header.data() + cli_directory);
    const std::uint32_t cli_size = read_le32(header.data() + cli_directory + 4);
    if (cli_rva == 0 || cli_size < 0x48) {
        return false;
    }

    std::uint32_t cli_offset = 0;
    const std::uint32_t size_of_headers = read_le32(header.data() + optional + 60);
    if (cli_rva < size_of_headers) {
        cli_offset = cli_rva;
    } else {
        const std::uint16_t section_count = read_le16(header.data() + pe_offset + 6);
        const std::size_t section_table = optional + optional_size;
        if (section_table > amount ||
            section_count > (amount - section_table) / 40) {
            return false;
        }

        bool mapped = false;
        for (std::uint16_t i = 0; i < section_count; ++i) {
            const std::uint8_t* section =
                header.data() + section_table + static_cast<std::size_t>(i) * 40;
            const std::uint32_t virtual_size = read_le32(section + 8);
            const std::uint32_t virtual_address = read_le32(section + 12);
            const std::uint32_t raw_size = read_le32(section + 16);
            const std::uint32_t raw_offset = read_le32(section + 20);
            const std::uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
            if (cli_rva < virtual_address) {
                continue;
            }
            const std::uint32_t relative = cli_rva - virtual_address;
            if (relative < span && relative < raw_size &&
                raw_offset <= UINT32_MAX - relative) {
                cli_offset = raw_offset + relative;
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            return false;
        }
    }

    std::array<std::uint8_t, 24> cli_header{};
    if (!package.read_range(entry, cli_offset, cli_header.data(),
                            cli_header.size(), error) ||
        read_le32(cli_header.data()) < 0x48) {
        return false;
    }

    const std::uint32_t flags = read_le32(cli_header.data() + 16);
    return (flags & kComImageFlagsIlOnly) != 0 &&
           (flags & (kComImageFlags32BitRequired |
                     kComImageFlagsNativeEntryPoint)) == 0;
}

bool has_prefix(const StfsPackage& package, const StfsEntry& entry,
                const char* expected, std::size_t length,
                std::string& error) {
    std::array<std::uint8_t, 8> prefix{};
    if (length > prefix.size() || entry.size < length ||
        !package.read_range(entry, 0, prefix.data(), length, error)) {
        return false;
    }
    return std::memcmp(prefix.data(), expected, length) == 0;
}

} // namespace

GameImageReport inspect_game_image(const StfsPackage& package) {
    GameImageReport report;

    for (const StfsEntry& entry : package.entries()) {
        if (!entry.is_directory) {
            ++report.file_count;
            if (entry.path.rfind("584E07D1/Content/", 0) == 0) {
                ++report.content_file_count;
            }
        }
    }

    bool sizes_match = true;
    for (const RequiredFile& required : kRequiredFiles) {
        const StfsEntry* entry = package.find(required.path);
        if (entry == nullptr || entry->is_directory) {
            report.missing_files.emplace_back(required.path);
            continue;
        }
        if (entry->size != required.expected_size) {
            sizes_match = false;
            report.problems.emplace_back(std::string(required.path) +
                                         " has an unsupported size");
        }
    }

    report.recognized = package.find("584E07D1/CastleMinerZ.exe") != nullptr &&
                        package.find("584E07D1/DNA.Common.dll") != nullptr;

    std::string error;
    const StfsEntry* executable = package.find("584E07D1/CastleMinerZ.exe");
    if (executable != nullptr) {
        report.managed_executable =
            is_il_only_anycpu_pe(package, *executable, error);
        if (!report.managed_executable) {
            report.problems.emplace_back(
                error.empty() ? "CastleMinerZ.exe is not an IL-only PE image"
                              : "Could not inspect CastleMinerZ.exe: " + error);
        }
    }

    const StfsEntry* load_screen = package.find("584E07D1/Content/LoadScreen.xnb");
    if (load_screen != nullptr &&
        !has_prefix(package, *load_screen, "XNBx", 4, error)) {
        report.problems.emplace_back("LoadScreen.xnb has an unexpected header");
    }

    report.supported_build = report.recognized && report.managed_executable &&
                             report.missing_files.empty() && sizes_match &&
                             report.file_count == 239;
    report.assets_ready = report.supported_build && report.problems.empty();
    return report;
}

std::string content_directory_path(const StfsPackage& package) {
    constexpr const char kContent[] = "Content";
    constexpr std::size_t kContentLength = sizeof(kContent) - 1;

    for (const StfsEntry& entry : package.entries()) {
        if (!entry.is_directory) {
            continue;
        }
        if (entry.path == kContent) {
            return entry.path;
        }
        if (entry.path.size() > kContentLength &&
            entry.path.compare(entry.path.size() - kContentLength,
                               kContentLength, kContent) == 0 &&
            entry.path[entry.path.size() - kContentLength - 1] == '/') {
            return entry.path;
        }
    }
    return std::string();
}

} // namespace cmz
