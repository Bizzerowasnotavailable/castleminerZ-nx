#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cmz {

struct StfsEntry {
    std::uint16_t table_index = 0;
    std::uint16_t parent_index = 0xFFFF;
    std::uint32_t size = 0;
    std::uint32_t start_block = 0;
    std::uint32_t block_count = 0;
    bool is_directory = false;
    bool is_contiguous = false;
    std::string name;
    std::string path;
};

class StfsPackage {
public:
    StfsPackage() = default;
    ~StfsPackage();

    StfsPackage(const StfsPackage&) = delete;
    StfsPackage& operator=(const StfsPackage&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    bool is_open() const { return file_ != nullptr; }
    const std::string& package_path() const { return package_path_; }
    const std::string& package_magic() const { return package_magic_; }
    const std::vector<StfsEntry>& entries() const { return entries_; }

    const StfsEntry* find(const std::string& path) const;

    // Exact match first, then a case-insensitive match. STFS package paths are
    // matched by the host filesystem case-insensitively on Xbox 360 and by
    // XNA/XACT content APIs that do not preserve case, so the Switch filesystem
    // view has to tolerate case differences as well.
    const StfsEntry* find_ignore_case(const std::string& path) const;
    bool read(const StfsEntry& entry, std::vector<std::uint8_t>& output,
              std::string& error) const;
    bool read_range(const StfsEntry& entry, std::size_t offset,
                    void* output, std::size_t length,
                    std::string& error) const;
    bool extract(const StfsEntry& entry, const std::string& output_path,
                 std::string& error) const;

    std::uint64_t file_size() const { return file_size_; }
    std::uint32_t data_start() const { return data_start_; }

    static bool has_stfs_magic(const std::string& path);

private:
    static std::uint64_t block_adjustment(std::uint32_t block,
                                          std::uint32_t hash_stride);
    static std::string normalize_path(const std::string& path);
    static bool safe_component(const std::string& name);

    bool seek_and_read(std::uint64_t offset, void* output, std::size_t length,
                       std::string& error) const;

    mutable std::FILE* file_ = nullptr;
    std::string package_path_;
    std::string package_magic_;
    std::vector<StfsEntry> entries_;
    std::uint64_t file_size_ = 0;
    std::uint32_t data_start_ = 0;
    std::uint32_t hash_stride_ = 0;
};

} // namespace cmz
