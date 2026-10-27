#include "cmz/game_image.hpp"
#include "cmz/stfs.hpp"

#include <cinttypes>
#include <cstdio>
#include <string>

namespace {

void usage(const char* program) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s list <STFS package>\n"
        "  %s verify <STFS package>\n"
        "  %s extract <STFS package> <package path> <output file>\n",
        program, program, program);
}

bool open_package(cmz::StfsPackage& package, const char* path) {
    std::string error;
    if (!package.open(path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    return true;
}

int list(const char* path) {
    cmz::StfsPackage package;
    if (!open_package(package, path)) {
        return 2;
    }
    std::printf("%s package, %" PRIu64 " bytes, %zu entries\n",
                package.package_magic().c_str(), package.file_size(),
                package.entries().size());
    for (const cmz::StfsEntry& entry : package.entries()) {
        std::printf("%s %10" PRIu32 "  block=%-7" PRIu32 "  %s%s\n",
                    entry.is_directory ? "DIR " : "FILE", entry.size,
                    entry.start_block, entry.path.c_str(),
                    (!entry.is_directory && !entry.is_contiguous) ?
                        " [fragmented]" : "");
    }
    return 0;
}

int verify(const char* path) {
    cmz::StfsPackage package;
    if (!open_package(package, path)) {
        return 2;
    }
    const cmz::GameImageReport report = cmz::inspect_game_image(package);
    std::printf("Package type:       %s\n", package.package_magic().c_str());
    std::printf("Files:              %zu\n", report.file_count);
    std::printf("Content files:      %zu\n", report.content_file_count);
    std::printf("CastleMiner Z:      %s\n", report.recognized ? "yes" : "no");
    std::printf("Managed executable: %s\n", report.managed_executable ? "yes" : "no");
    std::printf("Supported build:    %s\n", report.supported_build ? "yes" : "no");
    std::printf("Assets ready:       %s\n", report.assets_ready ? "yes" : "no");

    for (const std::string& missing : report.missing_files) {
        std::printf("missing: %s\n", missing.c_str());
    }
    for (const std::string& problem : report.problems) {
        std::printf("problem: %s\n", problem.c_str());
    }
    return report.assets_ready ? 0 : 3;
}

int extract(const char* package_path, const char* internal_path,
            const char* output_path) {
    cmz::StfsPackage package;
    if (!open_package(package, package_path)) {
        return 2;
    }
    const cmz::StfsEntry* entry = package.find(internal_path);
    if (entry == nullptr) {
        std::fprintf(stderr, "error: package path not found: %s\n", internal_path);
        return 3;
    }
    std::string error;
    if (!package.extract(*entry, output_path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 4;
    }
    std::printf("Extracted %s (%" PRIu32 " bytes)\n",
                entry->path.c_str(), entry->size);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "list") {
        return list(argv[2]);
    }
    if (argc == 3 && std::string(argv[1]) == "verify") {
        return verify(argv[2]);
    }
    if (argc == 5 && std::string(argv[1]) == "extract") {
        return extract(argv[2], argv[3], argv[4]);
    }
    usage(argv[0]);
    return 1;
}
