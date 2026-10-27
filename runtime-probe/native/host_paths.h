// Host-provided filesystem roots for SDL and FNA.
//
// devkitPro's SDL2 has no Switch filesystem backend: SDL_GetBasePath and
// SDL_GetPrefPath are stubs that return NULL. FNA uses the former as
// TitleLocation, which is the directory that ContentManager.RootDirectory is
// resolved against, so it has to point at the mounted game package instead of
// the NRO directory. sdl_paths.cpp replaces those two SDL entry points at link
// time; this header is how the host tells them where the game lives.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// base_directory must end with a slash and stay valid for the process
// lifetime, for example "stfs:/584E07D1/". pref_directory receives FNA/XNA
// storage and save data, for example "sdmc:/switch/castleminerz/".
void cmz_host_set_paths(const char* base_directory, const char* pref_directory);

const char* cmz_host_base_directory(void);
const char* cmz_host_pref_directory(void);

#ifdef __cplusplus
}
#endif
