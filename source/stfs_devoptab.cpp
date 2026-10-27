// Read-only devoptab over an STFS package, so the game sees the package as a
// filesystem instead of this project copying or unpacking game data.
//
// Switch only: the implementation is newlib's devoptab ABI (sys/iosupport.h),
// which is what mono-nx's System.IO ends up calling.

#include "cmz/package_mount.hpp"

#ifdef __SWITCH__

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <string>
#include <vector>

namespace cmz {
namespace {

constexpr const char* kDeviceName = "stfs";
constexpr std::uint16_t kRootParent = 0xFFFF;
constexpr off_t kBlockSize = 0x1000;

struct FileState {
    const StfsEntry* entry;
    std::uint64_t position;
};

struct DirState {
    std::uint16_t parent_index;
    std::uint16_t reserved;
    std::uint32_t cursor;
};

std::mutex g_mutex;
StfsPackage* g_package = nullptr;
devoptab_t g_device = {};

// newlib hands devoptab callbacks the full path, device prefix included.
// libnx's own fsdev strips "sdmc:" and friends itself, so this device does the
// same for "stfs:" instead of assuming newlib already did it.
std::string normalize(const char* path) {
    std::string result;
    if (path == nullptr) {
        return result;
    }

    constexpr const char* kPrefix = "stfs:";
    if (std::strncmp(path, kPrefix, 5) == 0) {
        path += 5;
    }

    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        const char value = *cursor == '\\' ? '/' : *cursor;
        if (value == '/' && (result.empty() || result.back() == '/')) {
            continue;
        }
        result.push_back(value);
    }

    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

const StfsEntry* resolve(const char* path) {
    if (g_package == nullptr) {
        return nullptr;
    }
    return g_package->find_ignore_case(normalize(path));
}

const StfsEntry* resolve_directory(const char* path) {
    const StfsEntry* entry = resolve(path);
    if (entry != nullptr && entry->is_directory) {
        return entry;
    }
    return nullptr;
}

void fill_stat(const StfsEntry& entry, struct stat* output) {
    std::memset(output, 0, sizeof(*output));
    output->st_mode = entry.is_directory ? (S_IFDIR | 0755) : (S_IFREG | 0444);
    output->st_size = static_cast<off_t>(entry.size);
    output->st_nlink = 1;
    output->st_blksize = kBlockSize;
}

int stfs_open(struct _reent* r, void* file_struct, const char* path,
              int flags, int mode) {
    (void)mode;

    if (g_package == nullptr) {
        r->_errno = ENODEV;
        return -1;
    }

    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0) {
        r->_errno = EROFS;
        return -1;
    }

    const StfsEntry* entry = resolve(path);
    if (entry == nullptr) {
        r->_errno = ENOENT;
        return -1;
    }
    if (entry->is_directory) {
        r->_errno = EISDIR;
        return -1;
    }

    auto* state = static_cast<FileState*>(file_struct);
    state->entry = entry;
    state->position = 0;
    return 0;
}

int stfs_close(struct _reent* r, void* file_struct) {
    (void)r;
    auto* state = static_cast<FileState*>(file_struct);
    state->entry = nullptr;
    state->position = 0;
    return 0;
}

ssize_t stfs_write(struct _reent* r, void* file_struct, const char* buffer,
                   size_t length) {
    (void)file_struct;
    (void)buffer;
    (void)length;
    r->_errno = EROFS;
    return -1;
}

ssize_t stfs_read(struct _reent* r, void* file_struct, char* buffer,
                  size_t length) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->entry == nullptr || g_package == nullptr) {
        r->_errno = EBADF;
        return -1;
    }

    const std::uint64_t remaining = state->entry->size - state->position;
    const std::size_t wanted = static_cast<std::size_t>(
        remaining < length ? remaining : length);
    if (wanted == 0) {
        return 0;
    }

    std::string error;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_package->read_range(*state->entry,
                               static_cast<std::size_t>(state->position),
                               buffer, wanted, error)) {
        r->_errno = EIO;
        return -1;
    }

    state->position += wanted;
    return static_cast<ssize_t>(wanted);
}

off_t stfs_seek(struct _reent* r, void* file_struct, off_t position,
                int whence) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->entry == nullptr) {
        r->_errno = EBADF;
        return -1;
    }

    const off_t size = static_cast<off_t>(state->entry->size);
    off_t target = position;
    if (whence == SEEK_CUR) {
        target = static_cast<off_t>(state->position) + position;
    } else if (whence == SEEK_END) {
        target = size + position;
    } else if (whence != SEEK_SET) {
        r->_errno = EINVAL;
        return -1;
    }

    if (target < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    if (target > size) {
        target = size;
    }

    state->position = static_cast<std::uint64_t>(target);
    return target;
}

int stfs_fstat(struct _reent* r, void* file_struct, struct stat* output) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->entry == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    fill_stat(*state->entry, output);
    return 0;
}

int stfs_stat(struct _reent* r, const char* path, struct stat* output) {
    if (g_package == nullptr) {
        r->_errno = ENODEV;
        return -1;
    }

    const std::string wanted = normalize(path);
    if (wanted.empty()) {
        std::memset(output, 0, sizeof(*output));
        output->st_mode = S_IFDIR | 0755;
        output->st_nlink = 1;
        return 0;
    }

    const StfsEntry* entry = resolve(path);
    if (entry == nullptr) {
        r->_errno = ENOENT;
        return -1;
    }
    fill_stat(*entry, output);
    return 0;
}

int stfs_unlink(struct _reent* r, const char* path) {
    (void)path;
    r->_errno = EROFS;
    return -1;
}

int stfs_rename(struct _reent* r, const char* old_path, const char* new_path) {
    (void)old_path;
    (void)new_path;
    r->_errno = EROFS;
    return -1;
}

int stfs_mkdir(struct _reent* r, const char* path, int mode) {
    (void)path;
    (void)mode;
    r->_errno = EROFS;
    return -1;
}

DIR_ITER* stfs_diropen(struct _reent* r, DIR_ITER* dir_state, const char* path) {
    if (g_package == nullptr) {
        r->_errno = ENODEV;
        return nullptr;
    }

    auto* state = static_cast<DirState*>(dir_state->dirStruct);
    const std::string wanted = normalize(path);
    if (wanted.empty()) {
        state->parent_index = kRootParent;
    } else {
        const StfsEntry* entry = resolve_directory(path);
        if (entry == nullptr) {
            r->_errno = ENOTDIR;
            return nullptr;
        }
        state->parent_index = entry->table_index;
    }

    state->reserved = 0;
    state->cursor = 0;
    return dir_state;
}

int stfs_dirreset(struct _reent* r, DIR_ITER* dir_state) {
    (void)r;
    auto* state = static_cast<DirState*>(dir_state->dirStruct);
    state->cursor = 0;
    return 0;
}

int stfs_dirnext(struct _reent* r, DIR_ITER* dir_state, char* filename,
                 struct stat* output) {
    if (g_package == nullptr) {
        r->_errno = ENODEV;
        return -1;
    }

    auto* state = static_cast<DirState*>(dir_state->dirStruct);
    const std::vector<StfsEntry>& entries = g_package->entries();

    while (state->cursor < entries.size()) {
        const StfsEntry& entry = entries[state->cursor++];
        if (entry.parent_index != state->parent_index) {
            continue;
        }

        std::snprintf(filename, NAME_MAX, "%s", entry.name.c_str());
        fill_stat(entry, output);
        return 0;
    }

    r->_errno = ENOENT;
    return -1;
}

int stfs_dirclose(struct _reent* r, DIR_ITER* dir_state) {
    (void)r;
    auto* state = static_cast<DirState*>(dir_state->dirStruct);
    state->parent_index = kRootParent;
    state->cursor = 0;
    return 0;
}

int stfs_statvfs(struct _reent* r, const char* path, struct statvfs* output) {
    (void)path;
    if (g_package == nullptr) {
        r->_errno = ENODEV;
        return -1;
    }

    std::memset(output, 0, sizeof(*output));
    const auto block_size = static_cast<unsigned long>(kBlockSize);
    output->f_bsize = block_size;
    output->f_frsize = block_size;
    output->f_blocks = static_cast<fsblkcnt_t>(g_package->file_size() / kBlockSize);
    output->f_bfree = 0;
    output->f_bavail = 0;
    output->f_files = static_cast<fsfilcnt_t>(g_package->entries().size());
    output->f_ffree = 0;
    output->f_namemax = 255;
    output->f_flag = ST_RDONLY;
    return 0;
}

void initialize_device() {
    std::memset(&g_device, 0, sizeof(g_device));
    g_device.name = kDeviceName;
    g_device.structSize = sizeof(FileState);
    g_device.open_r = stfs_open;
    g_device.close_r = stfs_close;
    g_device.write_r = stfs_write;
    g_device.read_r = stfs_read;
    g_device.seek_r = stfs_seek;
    g_device.fstat_r = stfs_fstat;
    g_device.stat_r = stfs_stat;
    g_device.unlink_r = stfs_unlink;
    g_device.rename_r = stfs_rename;
    g_device.mkdir_r = stfs_mkdir;
    g_device.dirStateSize = sizeof(DirState);
    g_device.diropen_r = stfs_diropen;
    g_device.dirreset_r = stfs_dirreset;
    g_device.dirnext_r = stfs_dirnext;
    g_device.dirclose_r = stfs_dirclose;
    g_device.statvfs_r = stfs_statvfs;
    g_device.deviceData = nullptr;
}

} // namespace

bool mount_package_device(StfsPackage& package, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_package != nullptr) {
        error = "the stfs device is already mounted";
        return false;
    }
    if (!package.is_open()) {
        error = "cannot mount a package that is not open";
        return false;
    }

    initialize_device();
    g_package = &package;

    const int index = AddDevice(&g_device);
    if (index < 0) {
        g_package = nullptr;
        error = "libnx rejected the stfs devoptab";
        return false;
    }

    error.clear();
    return true;
}

void unmount_package_device() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_package == nullptr) {
        return;
    }

    RemoveDevice(kDeviceName);
    g_package = nullptr;
}

} // namespace cmz

#else

namespace cmz {

bool mount_package_device(StfsPackage& package, std::string& error) {
    (void)package;
    error = "the stfs device is only available on Nintendo Switch";
    return false;
}

void unmount_package_device() {}

} // namespace cmz

#endif
