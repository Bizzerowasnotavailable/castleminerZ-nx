// A single Unix-style root for managed System.IO.
//
// mono-nx's System.Private.CoreLib recognizes only "/", "sdmc:/" and "romfs:/"
// as rooted paths (PathInternal.LibnxPathRoots). Any other device prefix is
// treated as relative, so a path such as "stfs:/GameInfo.xml" is silently
// rewritten to "<cwd>/stfs:/GameInfo.xml" before it reaches newlib.
//
// Managed code therefore sees "/game", "/save", "/logs" and "/data" instead,
// and this devoptab forwards each subtree to the real device. It also keeps the
// package read-only while giving saves a writable directory on the SD card.
//
// Switch only: the implementation is newlib's devoptab ABI.

#include "cmz/root_mount.hpp"

#ifdef __SWITCH__

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <string>

namespace cmz {
namespace {

constexpr const char* kDeviceName = "cmz";
constexpr const char* kSaveRoot = "sdmc:/switch/castleminerz/save";
constexpr const char* kLogsRoot = "sdmc:/switch/castleminerz-logs";
constexpr const char* kDataRoot = "sdmc:/switch/castleminerz/data";
constexpr std::uint32_t kBlockSize = 0x1000;

const char* const kRootEntries[] = {"game", "save", "logs", "data"};

std::string g_game_base;
devoptab_t g_device = {};

struct FileState {
    const devoptab_t* target;
    void* target_file;
};

struct DirState {
    bool synthetic_root;
    std::uint32_t cursor;
    const devoptab_t* target;
    DIR_ITER inner;
};

// Strips the "cmz:" device prefix, normalizes separators and removes duplicate
// and trailing slashes. The result is always absolute (starts with '/').
std::string normalize(const char* path) {
    if (path == nullptr) {
        return "/";
    }

    if (std::strncmp(path, "cmz:", 4) == 0) {
        path += 4;
    }

    std::string result;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        const char value = *cursor == '\\' ? '/' : *cursor;
        if (value == '/' && (result.empty() || result.back() == '/')) {
            continue;
        }
        result.push_back(value);
    }

    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }

    if (result.empty() || result.front() != '/') {
        result.insert(result.begin(), '/');
    }
    return result;
}

bool starts_with_segment(const std::string& path, const char* root) {
    const std::size_t length = std::strlen(root);
    if (path.compare(0, length, root) != 0) {
        return false;
    }
    return path.size() == length || path[length] == '/';
}

std::string join(const std::string& base, const std::string& suffix) {
    if (suffix.empty()) {
        return base;
    }
    if (suffix.front() == '/') {
        return base + suffix;
    }
    return base + "/" + suffix;
}

// Maps a managed path to the real path understood by the target devoptab.
// Returns false for the synthetic "/" directory.
bool map_path(const std::string& path, std::string& mapped) {
    if (path == "/") {
        return false;
    }

    if (starts_with_segment(path, "/game")) {
        mapped = join(g_game_base, path.substr(5));
        return true;
    }
    if (starts_with_segment(path, "/save")) {
        mapped = join(kSaveRoot, path.substr(5));
        return true;
    }
    if (starts_with_segment(path, "/logs")) {
        mapped = join(kLogsRoot, path.substr(5));
        return true;
    }
    if (starts_with_segment(path, "/data")) {
        mapped = join(kDataRoot, path.substr(5));
        return true;
    }

    // Relative paths that System.IO resolved against "cmz:/" land here. Treat
    // them as game-root relative, which is how an Xbox game sees its package.
    mapped = join(g_game_base, path);
    return true;
}

const devoptab_t* target_for(struct _reent* r, const std::string& mapped) {
    const int index = FindDevice(mapped.c_str());
    if (index < 0) {
        r->_errno = ENOENT;
        return nullptr;
    }

    const devoptab_t* target = devoptab_list[index];
    if (target == nullptr) {
        r->_errno = ENODEV;
        return nullptr;
    }
    return target;
}

int root_open(struct _reent* r, void* file_struct, const char* path,
              int flags, int mode) {
    const std::string wanted = normalize(path);
    std::string mapped;
    if (!map_path(wanted, mapped)) {
        r->_errno = EISDIR;
        return -1;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->open_r == nullptr) {
        r->_errno = ENOSYS;
        return -1;
    }

    const std::size_t size = target->structSize != 0 ? target->structSize : 1;
    void* target_file = std::calloc(1, size);
    if (target_file == nullptr) {
        r->_errno = ENOMEM;
        return -1;
    }

    if (target->open_r(r, target_file, mapped.c_str(), flags, mode) < 0) {
        std::free(target_file);
        return -1;
    }

    auto* state = static_cast<FileState*>(file_struct);
    state->target = target;
    state->target_file = target_file;
    return 0;
}

int root_close(struct _reent* r, void* file_struct) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target_file == nullptr) {
        return 0;
    }

    int result = 0;
    if (state->target->close_r != nullptr) {
        result = state->target->close_r(r, state->target_file);
    }
    std::free(state->target_file);
    state->target = nullptr;
    state->target_file = nullptr;
    return result;
}

ssize_t root_write(struct _reent* r, void* file_struct, const char* buffer,
                   size_t length) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->write_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->write_r(r, state->target_file, buffer, length);
}

ssize_t root_read(struct _reent* r, void* file_struct, char* buffer,
                  size_t length) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->read_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->read_r(r, state->target_file, buffer, length);
}

off_t root_seek(struct _reent* r, void* file_struct, off_t position,
                int whence) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->seek_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->seek_r(r, state->target_file, position, whence);
}

int root_fstat(struct _reent* r, void* file_struct, struct stat* output) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->fstat_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->fstat_r(r, state->target_file, output);
}

int root_ftruncate(struct _reent* r, void* file_struct, off_t length) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->ftruncate_r == nullptr) {
        r->_errno = ENOSYS;
        return -1;
    }
    return state->target->ftruncate_r(r, state->target_file, length);
}

int root_fsync(struct _reent* r, void* file_struct) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->fsync_r == nullptr) {
        r->_errno = ENOSYS;
        return -1;
    }
    return state->target->fsync_r(r, state->target_file);
}

void fill_directory_stat(struct stat* output) {
    std::memset(output, 0, sizeof(*output));
    output->st_mode = S_IFDIR | 0755;
    output->st_nlink = 1;
    output->st_blksize = static_cast<blksize_t>(kBlockSize);
}

int root_stat(struct _reent* r, const char* path, struct stat* output) {
    const std::string wanted = normalize(path);
    if (wanted == "/") {
        fill_directory_stat(output);
        return 0;
    }

    std::string mapped;
    if (!map_path(wanted, mapped)) {
        fill_directory_stat(output);
        return 0;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->stat_r == nullptr) {
        r->_errno = ENOSYS;
        return -1;
    }
    return target->stat_r(r, mapped.c_str(), output);
}

int root_lstat(struct _reent* r, const char* path, struct stat* output) {
    return root_stat(r, path, output);
}

int root_unlink(struct _reent* r, const char* path) {
    const std::string wanted = normalize(path);
    std::string mapped;
    if (!map_path(wanted, mapped)) {
        r->_errno = EISDIR;
        return -1;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->unlink_r == nullptr) {
        r->_errno = EROFS;
        return -1;
    }
    return target->unlink_r(r, mapped.c_str());
}

int root_rmdir(struct _reent* r, const char* path) {
    const std::string wanted = normalize(path);
    std::string mapped;
    if (!map_path(wanted, mapped)) {
        r->_errno = EBUSY;
        return -1;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->rmdir_r == nullptr) {
        r->_errno = EROFS;
        return -1;
    }
    return target->rmdir_r(r, mapped.c_str());
}

int root_rename(struct _reent* r, const char* old_path, const char* new_path) {
    std::string old_mapped;
    std::string new_mapped;
    if (!map_path(normalize(old_path), old_mapped) ||
        !map_path(normalize(new_path), new_mapped)) {
        r->_errno = EBUSY;
        return -1;
    }

    const devoptab_t* old_target = target_for(r, old_mapped);
    if (old_target == nullptr) {
        return -1;
    }
    const devoptab_t* new_target = target_for(r, new_mapped);
    if (new_target == nullptr) {
        return -1;
    }
    if (old_target != new_target) {
        r->_errno = EXDEV;
        return -1;
    }
    if (old_target->rename_r == nullptr) {
        r->_errno = EROFS;
        return -1;
    }
    return old_target->rename_r(r, old_mapped.c_str(), new_mapped.c_str());
}

int root_mkdir(struct _reent* r, const char* path, int mode) {
    const std::string wanted = normalize(path);
    std::string mapped;
    if (!map_path(wanted, mapped)) {
        r->_errno = EEXIST;
        return -1;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->mkdir_r == nullptr) {
        r->_errno = EROFS;
        return -1;
    }
    return target->mkdir_r(r, mapped.c_str(), mode);
}

int root_chmod(struct _reent* r, const char* path, mode_t mode) {
    std::string mapped;
    if (!map_path(normalize(path), mapped)) {
        r->_errno = EPERM;
        return -1;
    }
    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->chmod_r == nullptr) {
        r->_errno = EPERM;
        return -1;
    }
    return target->chmod_r(r, mapped.c_str(), mode);
}

int root_fchmod(struct _reent* r, void* file_struct, mode_t mode) {
    auto* state = static_cast<FileState*>(file_struct);
    if (state->target == nullptr || state->target->fchmod_r == nullptr) {
        r->_errno = EPERM;
        return -1;
    }
    return state->target->fchmod_r(r, state->target_file, mode);
}

int root_utimes(struct _reent* r, const char* path,
                const struct timeval times[2]) {
    std::string mapped;
    if (!map_path(normalize(path), mapped)) {
        r->_errno = EPERM;
        return -1;
    }
    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->utimes_r == nullptr) {
        r->_errno = EPERM;
        return -1;
    }
    return target->utimes_r(r, mapped.c_str(), times);
}

DIR_ITER* root_diropen(struct _reent* r, DIR_ITER* dir_state_r,
                       const char* path) {
    auto* state = static_cast<DirState*>(dir_state_r->dirStruct);
    state->synthetic_root = false;
    state->cursor = 0;
    state->target = nullptr;
    state->inner.device = -1;
    state->inner.dirStruct = nullptr;

    const std::string wanted = normalize(path);
    if (wanted == "/") {
        state->synthetic_root = true;
        return dir_state_r;
    }

    std::string mapped;
    if (!map_path(wanted, mapped)) {
        r->_errno = ENOTDIR;
        return nullptr;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return nullptr;
    }
    if (target->diropen_r == nullptr) {
        r->_errno = ENOTDIR;
        return nullptr;
    }

    const std::size_t size =
        target->dirStateSize != 0 ? target->dirStateSize : 1;
    state->inner.dirStruct = std::calloc(1, size);
    if (state->inner.dirStruct == nullptr) {
        r->_errno = ENOMEM;
        return nullptr;
    }
    state->inner.device = FindDevice(mapped.c_str());

    if (target->diropen_r(r, &state->inner, mapped.c_str()) == nullptr) {
        std::free(state->inner.dirStruct);
        state->inner.dirStruct = nullptr;
        return nullptr;
    }

    state->target = target;
    return dir_state_r;
}

int root_dirreset(struct _reent* r, DIR_ITER* dir_state_r) {
    auto* state = static_cast<DirState*>(dir_state_r->dirStruct);
    if (state->synthetic_root) {
        state->cursor = 0;
        return 0;
    }
    if (state->target == nullptr || state->target->dirreset_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->dirreset_r(r, &state->inner);
}

int root_dirnext(struct _reent* r, DIR_ITER* dir_state_r, char* filename,
                 struct stat* output) {
    auto* state = static_cast<DirState*>(dir_state_r->dirStruct);
    if (state->synthetic_root) {
        constexpr std::uint32_t count =
            sizeof(kRootEntries) / sizeof(kRootEntries[0]);
        if (state->cursor >= count) {
            r->_errno = ENOENT;
            return -1;
        }

        std::snprintf(filename, NAME_MAX, "%s", kRootEntries[state->cursor]);
        fill_directory_stat(output);
        state->cursor++;
        return 0;
    }

    if (state->target == nullptr || state->target->dirnext_r == nullptr) {
        r->_errno = EBADF;
        return -1;
    }
    return state->target->dirnext_r(r, &state->inner, filename, output);
}

int root_dirclose(struct _reent* r, DIR_ITER* dir_state_r) {
    auto* state = static_cast<DirState*>(dir_state_r->dirStruct);
    int result = 0;
    if (!state->synthetic_root && state->target != nullptr &&
        state->target->dirclose_r != nullptr) {
        result = state->target->dirclose_r(r, &state->inner);
    }

    std::free(state->inner.dirStruct);
    state->inner.dirStruct = nullptr;
    state->synthetic_root = false;
    state->cursor = 0;
    state->target = nullptr;
    return result;
}

int root_statvfs(struct _reent* r, const char* path, struct statvfs* output) {
    std::string mapped;
    if (!map_path(normalize(path), mapped)) {
        std::memset(output, 0, sizeof(*output));
        const auto block_size = static_cast<unsigned long>(kBlockSize);
        output->f_bsize = block_size;
        output->f_frsize = block_size;
        output->f_namemax = 255;
        return 0;
    }

    const devoptab_t* target = target_for(r, mapped);
    if (target == nullptr) {
        return -1;
    }
    if (target->statvfs_r == nullptr) {
        r->_errno = ENOSYS;
        return -1;
    }
    return target->statvfs_r(r, mapped.c_str(), output);
}

int root_chdir(struct _reent* r, const char* path) {
    (void)r;
    (void)path;
    // The root device is flat; accept any path so managed code can change
    // directories without failing. Relative paths still resolve through it.
    return 0;
}

void initialize_device() {
    std::memset(&g_device, 0, sizeof(g_device));
    g_device.name = kDeviceName;
    g_device.structSize = sizeof(FileState);
    g_device.open_r = root_open;
    g_device.close_r = root_close;
    g_device.write_r = root_write;
    g_device.read_r = root_read;
    g_device.seek_r = root_seek;
    g_device.fstat_r = root_fstat;
    g_device.stat_r = root_stat;
    g_device.lstat_r = root_lstat;
    g_device.unlink_r = root_unlink;
    g_device.rmdir_r = root_rmdir;
    g_device.rename_r = root_rename;
    g_device.mkdir_r = root_mkdir;
    g_device.chmod_r = root_chmod;
    g_device.fchmod_r = root_fchmod;
    g_device.utimes_r = root_utimes;
    g_device.chdir_r = root_chdir;
    g_device.dirStateSize = sizeof(DirState);
    g_device.diropen_r = root_diropen;
    g_device.dirreset_r = root_dirreset;
    g_device.dirnext_r = root_dirnext;
    g_device.dirclose_r = root_dirclose;
    g_device.statvfs_r = root_statvfs;
    g_device.ftruncate_r = root_ftruncate;
    g_device.fsync_r = root_fsync;
    g_device.deviceData = nullptr;
}

} // namespace

bool mount_root_device(const std::string& game_base, std::string& error) {
    if (!g_game_base.empty()) {
        error = "the cmz root device is already mounted";
        return false;
    }
    if (game_base.empty()) {
        error = "the game base directory is empty";
        return false;
    }

    g_game_base = game_base;
    while (g_game_base.size() > 1 && g_game_base.back() == '/') {
        g_game_base.pop_back();
    }

    initialize_device();
    if (AddDevice(&g_device) < 0) {
        g_game_base.clear();
        error = "libnx rejected the cmz root devoptab";
        return false;
    }

    error.clear();
    return true;
}

void unmount_root_device() {
    if (g_game_base.empty()) {
        return;
    }
    RemoveDevice(kDeviceName);
    g_game_base.clear();
}

} // namespace cmz

#else

namespace cmz {

bool mount_root_device(const std::string& game_base, std::string& error) {
    (void)game_base;
    error = "the cmz root device is only available on Nintendo Switch";
    return false;
}

void unmount_root_device() {}

} // namespace cmz

#endif
