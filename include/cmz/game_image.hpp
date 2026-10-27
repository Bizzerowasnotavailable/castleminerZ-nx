#pragma once

#include "cmz/stfs.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cmz {

struct GameImageReport {
    bool recognized = false;
    bool managed_executable = false;
    bool supported_build = false;
    bool assets_ready = false;
    std::size_t file_count = 0;
    std::size_t content_file_count = 0;
    std::vector<std::string> missing_files;
    std::vector<std::string> problems;
};

GameImageReport inspect_game_image(const StfsPackage& package);

// Package-relative path of the game's "Content" directory (for example
// "584E07D1/Content"), or an empty string when the package has no recognizable
// Content directory. FNA's TitleLocation is the directory that contains this,
// so ContentManager.RootDirectory = "Content" resolves through the mount.
std::string content_directory_path(const StfsPackage& package);

} // namespace cmz
