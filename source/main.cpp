#ifdef __SWITCH__

#include "cmz/game_image.hpp"
#include "cmz/package_mount.hpp"
#include "cmz/package_search.hpp"
#include "cmz/stfs.hpp"

#include <switch.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

namespace {

constexpr std::size_t kMaximumListedEntries = 8;
constexpr const char* kLogPath = "sdmc:/switch/castleminerz-logs/bootstrap.log";

std::FILE* g_log = nullptr;

// The console is the primary output, but the log lets a desktop-side test read
// the same lines without screenshotting the emulator.
void report(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);

    if (g_log != nullptr) {
        va_start(args, format);
        std::vfprintf(g_log, format, args);
        va_end(args);
        std::fflush(g_log);
    }
}

bool ensure_directory(const char* path) {
    if (mkdir(path, 0777) == 0) {
        return true;
    }
    return errno == EEXIST;
}

void list_root_entries() {
    DIR* handle = opendir("stfs:/");
    if (handle == nullptr) {
        report("VFS listing: opendir(stfs:/) failed\n");
        return;
    }

    std::size_t listed = 0;
    std::size_t total = 0;
    while (dirent* item = readdir(handle)) {
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        total++;
        if (listed < kMaximumListedEntries) {
            struct stat info {};
            stat(("stfs:/" + std::string(item->d_name)).c_str(), &info);
            report("  %s%s\n", item->d_name,
                   S_ISDIR(info.st_mode) ? "/" : "");
            listed++;
        }
    }
    closedir(handle);

    if (total > listed) {
        report("  ... and %zu more\n", total - listed);
    }
}

bool probe_read(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        report("VFS read: open failed (%s)\n", std::strerror(errno));
        return false;
    }

    char buffer[8] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);

    if (read != sizeof(buffer)) {
        report("VFS read: short read (%zu of %zu bytes)\n", read,
               sizeof(buffer));
        return false;
    }

    report("VFS read: %zu bytes from %s\n", read, path.c_str());
    return true;
}

void draw_status(int argc, char** argv, cmz::StfsPackage& package) {
    cmz::unmount_package_device();

    report("\x1b[2J\x1b[H");
    report("CastleMiner Z NX - bootstrap 0.1.0\n");
    report("===================================\n\n");

    std::string package_path;
    std::string error;
    cmz::GameImageReport image_report;
    if (!cmz::discover_game_package(argc, argv, package, package_path,
                                    image_report, error)) {
        report("No CastleMiner Z STFS package was found.\n\n");
        report("Copy your unmodified LIVE package below:\n");
        report("  sdmc:/switch/castleminerz/game/<package>\n\n");
        if (!error.empty()) {
            report("Last package error: %s\n\n", error.c_str());
        }
        report("Press X to rescan or + to exit.\n");
        return;
    }

    report("Package: %s\n", package_path.c_str());
    report("Files: %zu (%zu content files)\n",
           image_report.file_count, image_report.content_file_count);
    report("Managed IL executable: %s\n",
           image_report.managed_executable ? "yes" : "no");
    report("Known build: %s\n", image_report.supported_build ? "yes" : "no");
    report("Asset VFS: %s\n", image_report.assets_ready ? "ready" : "not ready");

    for (const std::string& missing : image_report.missing_files) {
        report("Missing: %s\n", missing.c_str());
    }
    for (const std::string& problem : image_report.problems) {
        report("Problem: %s\n", problem.c_str());
    }

    std::string mount_error;
    if (!cmz::mount_package_device(package, mount_error)) {
        report("\nVFS mount: failed (%s)\n", mount_error.c_str());
        report("\nPress X to rescan or + to exit.\n");
        return;
    }

    const std::string content = cmz::content_directory_path(package);
    report("\nVFS mount: stfs:/ (read-only)\n");
    if (content.empty()) {
        report("Content directory: not found\n");
    } else {
        report("Content directory: stfs:/%s\n", content.c_str());
    }

    report("\nPackage root:\n");
    list_root_entries();

    report("\n");
    probe_read("stfs:/GameInfo.xml");
    if (!content.empty()) {
        probe_read("stfs:/" + content + "/LoadScreen.xnb");
    }

    report("\nPress X to rescan or + to exit.\n");
}

} // namespace

int main(int argc, char** argv) {
    consoleInit(nullptr);

    (void)ensure_directory("sdmc:/switch");
    (void)ensure_directory("sdmc:/switch/castleminerz-logs");
    g_log = std::fopen(kLogPath, "w");

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    cmz::StfsPackage package;
    draw_status(argc, argv, package);
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad);
        if ((pressed & HidNpadButton_Plus) != 0) {
            break;
        }
        if ((pressed & HidNpadButton_X) != 0) {
            draw_status(argc, argv, package);
        }
        consoleUpdate(nullptr);
    }

    cmz::unmount_package_device();
    package.close();

    if (g_log != nullptr) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    consoleExit(nullptr);
    return 0;
}

#endif
