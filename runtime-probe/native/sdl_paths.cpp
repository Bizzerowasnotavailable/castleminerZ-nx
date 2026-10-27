// Link-time replacements for the two SDL2 filesystem entry points that the
// devkitPro Switch build does not implement.
//
// SDL2's nx filesystem backend is a stub: SDL_GetBasePath and SDL_GetPrefPath
// set SDL_Error(SDL_UNSUPPORTED) and return NULL. FNA treats a NULL base path as
// "ask the CLR", which on mono-nx resolves to the RomFS directory that holds
// program.dll, so ContentManager.RootDirectory = "Content" would look in the
// NRO image instead of the user's package.
//
// Both stubs live in SDL_sysfilesystem.o, which exports nothing else, so
// defining them here keeps that object out of the link entirely. The returned
// buffers are malloc'd because FNA hands them to SDL_free, which is free() in
// this build.
//
// This is a platform shim, not a fork: the pinned SDL2 source is untouched.

#include "host_paths.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

char g_base_directory[512];
char g_pref_directory[512];

char* duplicate(const char* value) {
    if (value[0] == '\0') {
        errno = ENOSYS;
        return nullptr;
    }

    const std::size_t length = std::strlen(value);
    auto* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }

    std::memcpy(copy, value, length + 1);
    return copy;
}

} // namespace

extern "C" void cmz_host_set_paths(const char* base_directory,
                                   const char* pref_directory) {
    std::snprintf(g_base_directory, sizeof(g_base_directory), "%s",
                  base_directory != nullptr ? base_directory : "");
    std::snprintf(g_pref_directory, sizeof(g_pref_directory), "%s",
                  pref_directory != nullptr ? pref_directory : "");
}

extern "C" const char* cmz_host_base_directory(void) {
    return g_base_directory;
}

extern "C" const char* cmz_host_pref_directory(void) {
    return g_pref_directory;
}

extern "C" char* SDL_GetBasePath(void) {
    // SDL's contract: the returned path always ends with a separator.
    return duplicate(g_base_directory);
}

extern "C" char* SDL_GetPrefPath(const char* org, const char* app) {
    (void)org;
    (void)app;
    // The host creates this directory before managed startup. SDL would also
    // create it here, but a second mkdir of an existing directory is noise.
    return duplicate(g_pref_directory);
}
