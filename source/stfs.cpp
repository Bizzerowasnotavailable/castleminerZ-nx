#include "cmz/stfs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace cmz {
namespace {

constexpr std::uint32_t kBlockSize = 0x1000;
constexpr std::uint64_t kMinimumPackageSize = 0xD000;
constexpr std::uint32_t kMinimumTableBlocks = 16;
constexpr std::uint32_t kMaximumTableBlocks = 4096;
constexpr std::size_t kEntrySize = 64;

std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint16_t read_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_le24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16);
}

std::string io_error(const char* operation) {
    std::string result(operation);
    result += ": ";
    result += std::strerror(errno);
    return result;
}

} // namespace

StfsPackage::~StfsPackage() {
    close();
}

bool StfsPackage::has_stfs_magic(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    std::array<char, 4> magic{};
    const bool read_ok = std::fread(magic.data(), 1, magic.size(), file) == magic.size();
    std::fclose(file);
    return read_ok &&
           (std::memcmp(magic.data(), "LIVE", 4) == 0 ||
            std::memcmp(magic.data(), "PIRS", 4) == 0 ||
            std::memcmp(magic.data(), "CON ", 4) == 0);
}

bool StfsPackage::open(const std::string& path, std::string& error) {
    close();
    error.clear();

    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        error = io_error("Unable to open STFS package");
        return false;
    }

    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = io_error("Unable to seek STFS package");
        close();
        return false;
    }

    const long end = std::ftell(file_);
    if (end < 0) {
        error = io_error("Unable to measure STFS package");
        close();
        return false;
    }
    file_size_ = static_cast<std::uint64_t>(end);

    if (file_size_ < kMinimumPackageSize) {
        error = "File is too small to be an STFS package";
        close();
        return false;
    }

    std::array<std::uint8_t, 4> magic{};
    if (!seek_and_read(0, magic.data(), magic.size(), error)) {
        close();
        return false;
    }
    if (std::memcmp(magic.data(), "LIVE", 4) != 0 &&
        std::memcmp(magic.data(), "PIRS", 4) != 0 &&
        std::memcmp(magic.data(), "CON ", 4) != 0) {
        error = "Unsupported package magic (expected LIVE, PIRS, or CON)";
        close();
        return false;
    }
    package_magic_.assign(reinterpret_cast<const char*>(magic.data()), magic.size());

    std::array<std::uint8_t, 2> path_indicator_bytes{};
    if (!seek_and_read(0xC032, path_indicator_bytes.data(),
                       path_indicator_bytes.size(), error)) {
        close();
        return false;
    }

    const std::uint16_t path_indicator = read_be16(path_indicator_bytes.data());
    data_start_ = path_indicator == 0xFFFF ? 0xC000 : 0xD000;
    hash_stride_ = data_start_ == 0xC000 ? 0x1000 : 0x2000;

    std::array<std::uint8_t, 2> first_cluster_bytes{};
    if (!seek_and_read(data_start_ + 0x2F, first_cluster_bytes.data(),
                       first_cluster_bytes.size(), error)) {
        close();
        return false;
    }

    std::uint32_t table_blocks = std::max<std::uint32_t>(
        read_le16(first_cluster_bytes.data()), kMinimumTableBlocks);
    const std::uint64_t available_blocks = (file_size_ - data_start_) / kBlockSize;
    if (table_blocks > kMaximumTableBlocks || table_blocks > available_blocks) {
        error = "STFS file table block count is outside safe bounds";
        close();
        return false;
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(table_blocks) * kBlockSize);
    if (!seek_and_read(data_start_, table.data(), table.size(), error)) {
        close();
        return false;
    }

    const std::size_t slot_count = table.size() / kEntrySize;
    std::vector<std::string> directory_paths(slot_count);
    std::vector<bool> directory_seen(slot_count, false);

    for (std::size_t i = 0; i < slot_count; ++i) {
        const std::uint8_t* raw = table.data() + i * kEntrySize;
        const std::uint8_t name_flags = raw[40];
        const std::size_t name_length = name_flags & 0x3F;
        if (name_length == 0) {
            break;
        }
        if (name_length > 40) {
            error = "STFS entry has an invalid name length";
            close();
            return false;
        }

        StfsEntry entry;
        entry.table_index = static_cast<std::uint16_t>(i);
        entry.name.assign(reinterpret_cast<const char*>(raw), name_length);
        entry.is_directory = (name_flags & 0x80) != 0;
        entry.is_contiguous = (name_flags & 0x40) != 0;
        entry.block_count = read_le24(raw + 41);
        const std::uint32_t secondary_block_count = read_le24(raw + 44);
        entry.start_block = read_le24(raw + 47);
        entry.parent_index = read_be16(raw + 50);
        entry.size = read_be32(raw + 52);

        if (!safe_component(entry.name)) {
            error = "STFS entry contains an unsafe path component";
            close();
            return false;
        }
        if (entry.block_count != secondary_block_count) {
            error = "STFS entry contains inconsistent block counts";
            close();
            return false;
        }

        std::string parent_path;
        if (entry.parent_index != 0xFFFF) {
            if (entry.parent_index >= directory_paths.size() ||
                !directory_seen[entry.parent_index]) {
                error = "STFS entry refers to an unknown parent directory";
                close();
                return false;
            }
            parent_path = directory_paths[entry.parent_index];
        }

        entry.path = parent_path.empty() ? entry.name : parent_path + "/" + entry.name;
        if (entry.is_directory) {
            directory_paths[i] = entry.path;
            directory_seen[i] = true;
        }
        entries_.push_back(std::move(entry));
    }

    if (entries_.empty()) {
        error = "STFS package contains no readable file-table entries";
        close();
        return false;
    }

    package_path_ = path;
    return true;
}

void StfsPackage::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    package_path_.clear();
    package_magic_.clear();
    entries_.clear();
    file_size_ = 0;
    data_start_ = 0;
    hash_stride_ = 0;
}

const StfsEntry* StfsPackage::find(const std::string& path) const {
    const std::string wanted = normalize_path(path);
    for (const StfsEntry& entry : entries_) {
        if (entry.path == wanted) {
            return &entry;
        }
    }
    return nullptr;
}

const StfsEntry* StfsPackage::find_ignore_case(const std::string& path) const {
    if (const StfsEntry* exact = find(path)) {
        return exact;
    }

    const std::string wanted = normalize_path(path);
    for (const StfsEntry& entry : entries_) {
        if (entry.path.size() != wanted.size()) {
            continue;
        }

        bool equal = true;
        for (std::size_t index = 0; index < wanted.size(); ++index) {
            const unsigned char left =
                static_cast<unsigned char>(entry.path[index]);
            const unsigned char right =
                static_cast<unsigned char>(wanted[index]);
            const unsigned char lower_left =
                left >= 'A' && left <= 'Z' ? left - 'A' + 'a' : left;
            const unsigned char lower_right =
                right >= 'A' && right <= 'Z' ? right - 'A' + 'a' : right;
            if (lower_left != lower_right) {
                equal = false;
                break;
            }
        }

        if (equal) {
            return &entry;
        }
    }
    return nullptr;
}

bool StfsPackage::read(const StfsEntry& entry,
                       std::vector<std::uint8_t>& output,
                       std::string& error) const {
    output.resize(entry.size);
    if (entry.size == 0) {
        error.clear();
        return !entry.is_directory;
    }
    if (!read_range(entry, 0, output.data(), output.size(), error)) {
        output.clear();
        return false;
    }
    return true;
}

bool StfsPackage::read_range(const StfsEntry& entry, std::size_t offset,
                             void* output, std::size_t length,
                             std::string& error) const {
    error.clear();
    if (file_ == nullptr) {
        error = "No STFS package is open";
        return false;
    }
    if (entry.is_directory) {
        error = "Cannot read a directory entry";
        return false;
    }
    if (!entry.is_contiguous && entry.size != 0) {
        error = "Fragmented STFS files are not implemented";
        return false;
    }
    if (offset > entry.size || length > static_cast<std::size_t>(entry.size) - offset) {
        error = "Requested range is outside the STFS entry";
        return false;
    }
    if (length == 0) {
        return true;
    }

    const std::uint64_t needed_blocks =
        (static_cast<std::uint64_t>(entry.size) + kBlockSize - 1) / kBlockSize;
    if (entry.block_count < needed_blocks) {
        error = "STFS entry does not contain enough blocks for its declared size";
        return false;
    }

    auto* destination = static_cast<std::uint8_t*>(output);
    std::size_t remaining = length;
    std::size_t file_offset = offset;
    while (remaining > 0) {
        const std::uint32_t relative_block = static_cast<std::uint32_t>(file_offset / kBlockSize);
        const std::size_t offset_in_block = file_offset % kBlockSize;
        const std::uint32_t logical_block = entry.start_block + relative_block;
        const std::uint64_t physical = static_cast<std::uint64_t>(data_start_) +
            static_cast<std::uint64_t>(logical_block) * kBlockSize +
            block_adjustment(logical_block, hash_stride_) + offset_in_block;
        const std::size_t amount = std::min<std::size_t>(
            remaining, kBlockSize - offset_in_block);

        if (!seek_and_read(physical, destination, amount, error)) {
            return false;
        }
        destination += amount;
        file_offset += amount;
        remaining -= amount;
    }
    return true;
}

bool StfsPackage::extract(const StfsEntry& entry,
                          const std::string& output_path,
                          std::string& error) const {
    if (entry.is_directory) {
        error = "Cannot extract a directory entry";
        return false;
    }

    std::FILE* output = std::fopen(output_path.c_str(), "wb");
    if (output == nullptr) {
        error = io_error("Unable to create output file");
        return false;
    }

    std::array<std::uint8_t, kBlockSize> buffer{};
    std::size_t offset = 0;
    bool success = true;
    while (offset < entry.size) {
        const std::size_t amount = std::min<std::size_t>(
            buffer.size(), static_cast<std::size_t>(entry.size) - offset);
        if (!read_range(entry, offset, buffer.data(), amount, error)) {
            success = false;
            break;
        }
        if (std::fwrite(buffer.data(), 1, amount, output) != amount) {
            error = io_error("Unable to write output file");
            success = false;
            break;
        }
        offset += amount;
    }

    if (std::fclose(output) != 0 && success) {
        error = io_error("Unable to close output file");
        success = false;
    }
    if (!success) {
        std::remove(output_path.c_str());
    }
    return success;
}

std::uint64_t StfsPackage::block_adjustment(std::uint32_t block,
                                            std::uint32_t hash_stride) {
    std::uint64_t result = 0;
    while (block >= 170) {
        block /= 170;
        result += static_cast<std::uint64_t>(block + 1) * hash_stride;
    }
    return result;
}

std::string StfsPackage::normalize_path(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    bool previous_slash = false;
    for (char value : path) {
        char c = value == '\\' ? '/' : value;
        if (c == '/') {
            if (result.empty() || previous_slash) {
                continue;
            }
            previous_slash = true;
        } else {
            previous_slash = false;
        }
        result.push_back(c);
    }
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

bool StfsPackage::safe_component(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

bool StfsPackage::seek_and_read(std::uint64_t offset, void* output,
                                std::size_t length, std::string& error) const {
    if (offset > file_size_ || length > file_size_ - offset) {
        error = "STFS read would pass the end of the package";
        return false;
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<long>::max())) {
        error = "STFS offset is not representable on this platform";
        return false;
    }
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        error = io_error("Unable to seek STFS package");
        return false;
    }
    if (length > 0 && std::fread(output, 1, length, file_) != length) {
        error = io_error("Unable to read STFS package");
        return false;
    }
    return true;
}

} // namespace cmz
