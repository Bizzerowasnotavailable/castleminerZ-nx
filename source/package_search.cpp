// Shared package discovery for the bootstrap NRO and the runtime host.
//
// Only the directory walk is Switch-specific (libnx/devkitA64 dirent + stat);
// the open/inspect/select policy is portable so it can be unit tested if a host
// test harness ever needs it.

#include "cmz/package_search.hpp"

#include <cstring>
#include <utility>

#ifdef __SWITCH__
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace cmz {
namespace {

bool has_override(int argc, char** argv, std::string& output) {
    if (argc <= 1 || argv == nullptr || argv[1] == nullptr || argv[1][0] == '\0') {
        return false;
    }
    output.assign(argv[1]);
    return true;
}

} // namespace

#ifdef __SWITCH__

namespace {

bool is_directory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

void walk(const std::string& directory, int depth,
          const PackageSearchOptions& options,
          std::vector<std::string>& output) {
    if (depth > options.maximum_depth ||
        output.size() >= options.maximum_candidates) {
        return;
    }

    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr) {
        return;
    }

    while (const dirent* item = readdir(handle)) {
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }

        const std::string path = directory + "/" + item->d_name;
        if (is_directory(path)) {
            walk(path, depth + 1, options, output);
        } else if (StfsPackage::has_stfs_magic(path)) {
            output.push_back(path);
        }

        if (output.size() >= options.maximum_candidates) {
            break;
        }
    }

    closedir(handle);
}

} // namespace

std::vector<std::string> find_package_candidates(
    const PackageSearchOptions& options) {
    std::vector<std::string> candidates;
    walk(options.root, 0, options, candidates);
    return candidates;
}

#else

std::vector<std::string> find_package_candidates(
    const PackageSearchOptions& options) {
    (void)options;
    return {};
}

#endif

bool open_game_package(const std::vector<std::string>& candidates,
                       StfsPackage& package, std::string& selected_path,
                       GameImageReport& report, std::string& last_error) {
    for (const std::string& path : candidates) {
        package.close();
        std::string error;
        if (!package.open(path, error)) {
            last_error = path + ": " + error;
            continue;
        }

        GameImageReport candidate = inspect_game_image(package);
        if (candidate.recognized) {
            selected_path = path;
            report = std::move(candidate);
            last_error.clear();
            return true;
        }

        package.close();
    }

    return false;
}

bool discover_game_package(int argc, char** argv, StfsPackage& package,
                           std::string& selected_path, GameImageReport& report,
                           std::string& last_error) {
    std::vector<std::string> candidates;

    std::string overridden;
    if (has_override(argc, argv, overridden)) {
        candidates.push_back(overridden);
    }

    for (std::string& candidate : find_package_candidates(PackageSearchOptions{})) {
        candidates.push_back(std::move(candidate));
    }

    return open_game_package(candidates, package, selected_path, report,
                             last_error);
}

} // namespace cmz
