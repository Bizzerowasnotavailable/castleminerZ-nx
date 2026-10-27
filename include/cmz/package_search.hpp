#pragma once

#include "cmz/game_image.hpp"
#include "cmz/stfs.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cmz {

struct PackageSearchOptions {
    std::string root = "sdmc:/switch/castleminerz/game";
    int maximum_depth = 6;
    std::size_t maximum_candidates = 128;
};

// Recursively collects files whose first four bytes are an STFS package magic.
std::vector<std::string> find_package_candidates(
    const PackageSearchOptions& options);

// Opens the first candidate that inspects as a recognized CastleMiner Z image.
// The package is left open on success and closed on failure.
bool open_game_package(const std::vector<std::string>& candidates,
                       StfsPackage& package, std::string& selected_path,
                       GameImageReport& report, std::string& last_error);

// argv[1] override plus the default search root.
bool discover_game_package(int argc, char** argv, StfsPackage& package,
                           std::string& selected_path, GameImageReport& report,
                           std::string& last_error);

} // namespace cmz
