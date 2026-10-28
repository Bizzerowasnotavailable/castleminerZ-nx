// newlib only declares POSIX functions such as setenv() when a feature level is
// requested; the pinned mono-nx Makefile compiles C++ in strict -std=c++17 mode.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

// castleminerZ-nx runtime host.
//
// Replaces mono-nx's sample native/aot/source/main.c. Before Mono starts it:
//
//   1. finds a CastleMiner Z STFS package on the SD card,
//   2. validates it and mounts it as the read-only "stfs:" devoptab,
//   3. points SDL's base/pref paths at the package and the SD card,
//   4. sets the FNA/FNA3D backend variables that FNA would otherwise guess,
//   5. then hands control to the static-AOT Mono runtime.
//
// The package is never copied or unpacked. Managed System.IO (and therefore
// FNA's TitleContainer/ContentManager) reaches it through the devoptab.

// The mono-nx host helpers are compiled as C (core.c), so give their
// declarations C linkage in this C++ translation unit.
extern "C" {
#include "core.h"
}

#include "host_paths.h"
#include "local_profile.h"

#include "cmz/game_image.hpp"
#include "cmz/package_mount.hpp"
#include "cmz/package_search.hpp"
#include "cmz/root_mount.hpp"
#include "cmz/stfs.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define STATIC_MONO_SYM(x) do {\
    extern void* x; \
    mono_aot_register_module((void**) x);\
} while(0)

namespace {

constexpr const char* kLogPath = "sdmc:/switch/castleminerz-logs/host.log";
constexpr const char* kPrefPath = "sdmc:/switch/castleminerz/";

std::FILE* g_log = nullptr;

void log_line(const char* format, ...) __attribute__((format(printf, 1, 2)));

void log_line(const char* format, ...) {
    if (g_log == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

bool ensure_directory(const char* path) {
    struct stat info {};
    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    return mkdir(path, 0777) == 0;
}

// "stfs:/584E07D1/Content" -> "stfs:/584E07D1/"; "Content" -> "stfs:/"
std::string base_directory_for(const std::string& content_directory) {
    const std::size_t separator = content_directory.rfind('/');
    if (separator == std::string::npos) {
        return "stfs:/";
    }
    return "stfs:/" + content_directory.substr(0, separator + 1);
}

// The managed root device exposes the package as "/game", so the content
// directory ("584E07D1/Content") becomes "/game/Content". The base directory is
// always the parent of the content directory, hence the last path segment.
std::string virtual_content_directory(const std::string& content_directory) {
    const std::size_t separator = content_directory.rfind('/');
    const std::string name = separator == std::string::npos
                                 ? content_directory
                                 : content_directory.substr(separator + 1);
    if (name.empty()) {
        return "/game";
    }
    return "/game/" + name;
}

void configure_environment() {
    // FNA picks SDL3 unless told otherwise, and its OpenGL backend only selects
    // GLES on a hard-coded platform list that does not include "Switch".
    setenv("FNA_PLATFORM_BACKEND", "SDL2", 1);
    // Match the game's A/B/X/Y prompts to Nintendo labels, not Xbox positions.
    // Without this, a physical Switch A is reported to the game as Xbox B.
    setenv("FNA_GAMEPAD_IGNORE_PHYSICAL_LAYOUT", "1", 1);
    // SDL 2.28.5's libnx backend has a fixed positional mapping that ignores
    // the label hint. Override only that backend's built-in controller GUID.
    setenv("SDL_GAMECONTROLLERCONFIG",
           "000038f853776974636820436f6e7400,Switch Controller (Nintendo labels),"
           "a:b0,b:b1,x:b2,y:b3,back:b11,start:b10,"
           "leftstick:b4,rightstick:b5,leftshoulder:b6,rightshoulder:b7,"
           "lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
           "dpleft:b12,dpup:b13,dpright:b14,dpdown:b15,crc:f838,", 1);
    setenv("FNA3D_FORCE_DRIVER", "OpenGL", 1);
    setenv("FNA3D_OPENGL_FORCE_ES3", "1", 1);
    setenv("FNA3D_OPENGL_WINDOW_DEPTHSTENCILFORMAT", "Depth24Stencil8", 1);
    setenv("CMZ_REPLACEMENT_EFFECT_DIRECTORY", "romfs:/effects", 1);
    // BlockEffect samples the first environment slice at its exact texel center.
    setenv("CMZ_FIRST_ENVIRONMENT_SLICE_ONLY", "1", 1);

    // The base path is provided by sdl_paths.cpp, so this only keeps FNA from
    // preferring AppDomain.CurrentDomain.BaseDirectory if the order changes.
    setenv("FNA_SDL_FORCE_BASE_PATH", "1", 1);
}

} // namespace

extern "C" {

// Consumed by the managed probe so it can report and exercise the mount. The
// strings have static storage duration and are never freed.
static char g_vfs_package_path[512];
static char g_vfs_content_directory[512];
static int g_vfs_mounted = 0;
static bool g_root_mounted = false;

int cmz_vfs_mounted(void) {
    return g_vfs_mounted;
}

const char* cmz_vfs_package_path(void) {
    return g_vfs_package_path;
}

const char* cmz_vfs_content_directory(void) {
    return g_vfs_content_directory;
}

// Lets managed code report progress without going through System.IO, whose
// path handling on this runtime is exactly what some probes need to diagnose.
void cmz_host_log(const char* message) {
    log_line("managed: %s", message != nullptr ? message : "(null)");
}

} // extern "C"

int main(int argc, char* argv[]) {
    romfsInit();

    const bool log_directory_ready =
        ensure_directory("sdmc:/switch") &&
        ensure_directory("sdmc:/switch/castleminerz-logs");
    if (log_directory_ready) {
        g_log = std::fopen(kLogPath, "w");
    }

    // The managed root device maps "/save" and "/data" onto the SD card. Create
    // both before managed code runs so FNA's pref path exists on first boot.
    ensure_directory("sdmc:/switch/castleminerz/save");
    ensure_directory("sdmc:/switch/castleminerz/data");
    const CmzLocalProfile local_profile = cmz_initialize_local_profile();
    log_line("castleminerZ-nx runtime host");
    log_line("local Switch profile: %s (no online account required)",
             local_profile.available ? "selected" : "unavailable; offline fallback");
    log_line("arguments: %d", argc - 1);
    for (int index = 1; index < argc; ++index) {
        log_line("  argv[%d] = %s", index, argv[index]);
    }

    // Configure FNA before any managed type touches it.
    configure_environment();
    log_line("controller buttons: Nintendo labels (A=accept, B=back)");

    if (!ensure_directory(kPrefPath)) {
        log_line("warning: could not create the storage directory (%s)",
                 std::strerror(errno));
    }

    // The package object lives for the whole process: open file handles in the
    // devoptab point into its entry table.
    static cmz::StfsPackage package;

    std::string package_path;
    std::string error;
    cmz::GameImageReport report;
    const bool found = cmz::discover_game_package(
        argc, argv, package, package_path, report, error);

    if (found) {
        log_line("package: %s", package_path.c_str());
        log_line("files: %zu (%zu content), known build: %s",
                 report.file_count, report.content_file_count,
                 report.supported_build ? "yes" : "no");

        std::string mount_error;
        if (cmz::mount_package_device(package, mount_error)) {
            const std::string content = cmz::content_directory_path(package);
            const std::string base = base_directory_for(content);
            const std::string virtual_content = virtual_content_directory(content);

            std::snprintf(g_vfs_package_path, sizeof(g_vfs_package_path), "%s",
                          package_path.c_str());
            g_vfs_mounted = 1;

            std::string root_error;
            if (cmz::mount_root_device(base, root_error)) {
                g_root_mounted = true;
                if (chdir("cmz:/") != 0) {
                    log_line("warning: chdir cmz:/ failed (%s)",
                             std::strerror(errno));
                }

                std::snprintf(g_vfs_content_directory,
                              sizeof(g_vfs_content_directory), "%s",
                              virtual_content.c_str());
                cmz_host_set_paths("/game/", local_profile.pref_directory.c_str());

                log_line("package root: cmz:/ -> %s", base.c_str());
                log_line("vfs mounted: %s", g_vfs_content_directory);
                log_line("SDL base path: %s", cmz_host_base_directory());
                log_line("SDL pref path: %s", cmz_host_pref_directory());
            } else {
                // Without the root device, managed "/..." paths resolve against
                // whichever device the working directory names. Keep the old
                // stfs: paths so the failure is still diagnosable in the log.
                log_line("root device mount failed: %s", root_error.c_str());
                std::snprintf(g_vfs_content_directory,
                              sizeof(g_vfs_content_directory), "stfs:/%s",
                              content.c_str());
                cmz_host_set_paths(base.c_str(), kPrefPath);
                log_line("vfs mounted without managed root: %s",
                         g_vfs_content_directory);
            }
        } else {
            log_line("vfs mount failed: %s", mount_error.c_str());
            cmz_host_set_paths("", kPrefPath);
        }
    } else {
        log_line("no CastleMiner Z package found");
        if (!error.empty()) {
            log_line("last error: %s", error.c_str());
        }
        cmz_host_set_paths("", kPrefPath);
    }

    // Managed System.IO only accepts "/", "sdmc:/" and "romfs:/" as rooted
    // paths, so the package lives behind the virtual "cmz:/" root device and
    // the working directory is its root. Without that device, fall back to the
    // RomFS working directory and the old absolute stfs: paths.
    if (!g_root_mounted) {
        chdir("romfs:/");
    }

    // A config on the SD card wins so runtime settings (Mono tracing, stdio
    // redirection) can be changed without rebuilding the NRO.
    constexpr const char* kSdConfigPath =
        "sdmc:/switch/castleminerz-logs/aot_config.ini";
    const char* config_path = "romfs:/aot_config.ini";
    if (access(kSdConfigPath, F_OK) == 0) {
        config_path = kSdConfigPath;
    }
    log_line("config: %s", config_path);

    log_line("application_initialize: begin");
    if (!application_initialize(config_path)) {
        log_line("application_initialize failed");
        return 1;
    }
    log_line("application_initialize: done");

    MonoDomain* domain = nullptr;

    // Output from build_aot.sh
    #include "mono_symbols.h"

    mono_jit_set_aot_mode(MONO_AOT_MODE_FULL);

    log_line("application_configure_mono: begin");
    application_configure_mono();
    log_line("mono_jit_init: begin");

    domain = mono_jit_init("embedded_mono");
    if (domain == nullptr) {
        log_line("mono_jit_init failed");
        fatal_error("Failed to initialize mono domain\n");
        return 1;
    }
    log_line("mono_jit_init: done");

    // Mono resolves static-AOT dependencies before mono_jit_exec establishes
    // AppContext.BaseDirectory. Preload the companion assembly explicitly so
    // it does not have to be duplicated in the runtime's RomFS search root.
    const std::string entry_path = g_config.default_assembly;
    const std::size_t entry_separator = entry_path.find_last_of("/\\");
    if (entry_path.substr(entry_separator == std::string::npos
                              ? 0 : entry_separator + 1) == "CastleMinerZ.exe") {
        const std::string companion = entry_path.substr(
            0, entry_separator == std::string::npos ? 0 : entry_separator + 1) +
            "DNA.Common.dll";
        log_line("companion assembly open: %s", companion.c_str());
        if (mono_domain_assembly_open(domain, companion.c_str()) == nullptr) {
            log_line("could not load companion assembly");
            fatal_error("Failed to load DNA.Common.dll from the managed SD directory");
            return 1;
        }
        log_line("companion assembly open: done");
    }

    io_debugf("Loading assembly %s", g_config.default_assembly);

    log_line("assembly open: %s", g_config.default_assembly);
    MonoAssembly* assembly =
        mono_domain_assembly_open(domain, g_config.default_assembly);
    if (assembly == nullptr) {
        log_line("could not load %s", g_config.default_assembly);
        fatal_error("Failed to load assembly");
        return 1;
    }
    log_line("assembly open: done");

    char* monoargs[] = {g_config.default_assembly};
    log_line("mono_jit_exec: begin");
    mono_jit_exec(domain, assembly, 1, monoargs);
    log_line("mono_jit_exec: done");

    mono_jit_cleanup(domain);

    cmz::unmount_root_device();
    cmz::unmount_package_device();
    package.close();

    application_terminate();
    romfsExit();

    log_line("runtime host exiting");
    if (g_log != nullptr) {
        std::fclose(g_log);
        g_log = nullptr;
    }

    return 0;
}
