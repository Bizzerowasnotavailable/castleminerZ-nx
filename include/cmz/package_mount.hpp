#pragma once

#include "cmz/stfs.hpp"

#include <string>

namespace cmz {

// Mounts a read-only devoptab named "stfs" over an already-open package, so
// managed code can open "stfs:/584E07D1/Content/..." through System.IO.File and
// through FNA's TitleContainer/ContentManager.
//
// The package object must outlive the mount and must not be reopened while the
// device is mounted: open file handles point into its entry table.
//
// Switch only. On other platforms this returns false and fills error.
bool mount_package_device(StfsPackage& package, std::string& error);
void unmount_package_device();

} // namespace cmz
