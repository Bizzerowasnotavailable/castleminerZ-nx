#pragma once

#include <string>

namespace cmz {

// Managed code on mono-nx only treats "/" (plus the built-in sdmc:/ and romfs:/)
// as a rooted path. Device paths invented by this project are not in that list,
// so System.IO joins them with the current directory and breaks them.
//
// This device gives managed code one ordinary Unix-style root instead:
//
//   /game/...  -> the package (game_base, for example "stfs:/584E07D1")
//   /save/...  -> sdmc:/switch/castleminerz/save
//   /logs/...  -> sdmc:/switch/castleminerz-logs
//   /data/...  -> sdmc:/switch/castleminerz/data
//
// Call chdir("cmz:/") after mounting so "/..." resolves to this device.
bool mount_root_device(const std::string& game_base, std::string& error);
void unmount_root_device();

} // namespace cmz
