#include "cmz/game_image.hpp"
#include "cmz/stfs.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kDataStart = 0xC000;
constexpr std::size_t kPackageSize = 0xC0000;

void write_be16(std::vector<unsigned char>& data, std::size_t offset,
                unsigned value) {
    data[offset] = static_cast<unsigned char>(value >> 8);
    data[offset + 1] = static_cast<unsigned char>(value);
}

void write_be32(std::vector<unsigned char>& data, std::size_t offset,
                unsigned value) {
    data[offset] = static_cast<unsigned char>(value >> 24);
    data[offset + 1] = static_cast<unsigned char>(value >> 16);
    data[offset + 2] = static_cast<unsigned char>(value >> 8);
    data[offset + 3] = static_cast<unsigned char>(value);
}

void write_le24(std::vector<unsigned char>& data, std::size_t offset,
                unsigned value) {
    data[offset] = static_cast<unsigned char>(value);
    data[offset + 1] = static_cast<unsigned char>(value >> 8);
    data[offset + 2] = static_cast<unsigned char>(value >> 16);
}

void write_entry(std::vector<unsigned char>& data, unsigned index,
                 const char* name, bool directory, unsigned parent,
                 unsigned start_block, unsigned size) {
    const std::size_t base = kDataStart + index * 64;
    const std::size_t name_length = std::strlen(name);
    std::memcpy(data.data() + base, name, name_length);
    data[base + 40] = static_cast<unsigned char>(name_length | 0x40 |
                                                 (directory ? 0x80 : 0));
    const unsigned blocks = size == 0 ? 0 : (size + 0xFFF) / 0x1000;
    write_le24(data, base + 41, blocks);
    write_le24(data, base + 44, blocks);
    write_le24(data, base + 47, start_block);
    write_be16(data, base + 50, parent);
    write_be32(data, base + 52, size);
}

bool create_package(const char* path) {
    std::vector<unsigned char> data(kPackageSize, 0);
    std::memcpy(data.data(), "LIVE", 4);
    write_be16(data, 0xC032, 0xFFFF);
    write_entry(data, 0, "root", true, 0xFFFF, 0, 0);
    write_entry(data, 1, "hello.bin", false, 0, 16, 5);
    write_entry(data, 2, "crosses-hash-table.bin", false, 0, 169, 0x1001);
    write_entry(data, 3, "Content", true, 0, 0, 0);
    write_entry(data, 4, "LoadScreen.xnb", false, 3, 32, 3);
    std::memcpy(data.data() + kDataStart + 16 * 0x1000, "hello", 5);
    std::memcpy(data.data() + kDataStart + 32 * 0x1000, "XNB", 3);
    std::memset(data.data() + kDataStart + 169 * 0x1000, 'A', 0x1000);
    data[kDataStart + 170 * 0x1000 + 2 * 0x1000] = 'Z';

    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    return std::fclose(file) == 0 && ok;
}

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main() {
    const char* path = "cmz_stfs_test_package.bin";
    if (!create_package(path)) {
        return fail("could not create synthetic package");
    }

    cmz::StfsPackage package;
    std::string error;
    if (!package.open(path, error)) {
        std::remove(path);
        std::fprintf(stderr, "open error: %s\n", error.c_str());
        return fail("synthetic package did not open");
    }
    if (package.package_magic() != "LIVE" || package.entries().size() != 5) {
        std::remove(path);
        return fail("package metadata mismatch");
    }

    const cmz::StfsEntry* entry = package.find("/root//hello.bin/");
    if (entry == nullptr || entry->size != 5) {
        std::remove(path);
        return fail("path lookup failed");
    }

    std::vector<unsigned char> bytes;
    if (!package.read(*entry, bytes, error) ||
        std::string(bytes.begin(), bytes.end()) != "hello") {
        std::remove(path);
        return fail("file read failed");
    }

    const cmz::StfsEntry* crossing = package.find("root/crosses-hash-table.bin");
    if (crossing == nullptr || !package.read(*crossing, bytes, error) ||
        bytes.size() != 0x1001 || bytes.front() != 'A' || bytes[0xFFF] != 'A' ||
        bytes.back() != 'Z') {
        std::remove(path);
        return fail("hash-table boundary read failed");
    }

    unsigned char boundary[3]{};
    if (!package.read_range(*crossing, 0xFFE, boundary, sizeof(boundary), error) ||
        boundary[0] != 'A' || boundary[1] != 'A' || boundary[2] != 'Z') {
        std::remove(path);
        return fail("partial boundary read failed");
    }
    if (package.read_range(*crossing, crossing->size, boundary, 1, error) ||
        error.empty()) {
        std::remove(path);
        return fail("out-of-range read was accepted");
    }

    if (package.find("ROOT/HELLO.BIN") != nullptr) {
        std::remove(path);
        return fail("exact lookup was unexpectedly case-insensitive");
    }
    const cmz::StfsEntry* insensitive = package.find_ignore_case("ROOT/Hello.Bin");
    if (insensitive == nullptr || insensitive->size != 5 ||
        std::string(insensitive->path) != "root/hello.bin") {
        std::remove(path);
        return fail("case-insensitive lookup failed");
    }
    if (cmz::content_directory_path(package) != "root/Content") {
        std::remove(path);
        return fail("content directory discovery failed");
    }

    std::remove(path);
    std::puts("PASS: STFS parser synthetic package test");
    return 0;
}
