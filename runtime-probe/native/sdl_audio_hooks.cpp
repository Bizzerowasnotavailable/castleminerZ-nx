// SDL audio hooks for the Switch build.
//
// 1. Audio thread priority. SDL's audio thread asks for
//    SDL_THREAD_PRIORITY_TIME_CRITICAL. SDL 2.28.5's libnx backend maps every
//    priority except HIGH to 0x3B, the lowest application priority, so FAudio's
//    mixer competed with the game's busy worker threads (also 0x3B) for CPU
//    time and could miss its deadline. TIME_CRITICAL now maps to 0x2A, above
//    the main thread (0x2C). The mixer runs for a short time per period and its
//    thread may use any core, so this cannot starve the game.
// 2. Diagnostics. SDL_OpenAudioDevice is recorded with the format it obtained,
//    and the device callback (FAudio's mixer) is timed and its output level
//    measured, so perf.log shows whether audio is produced at all. From the
//    first audio open on, SDL log messages (FAudio's and libavcodec's go
//    through SDL_Log) are also copied into perf.log, then passed on to the
//    previous log function.
//
// The linker routes SDL's own references through these functions with
// -Wl,--wrap (see build-game.ps1). Each wrapper calls the real function.

#include "sdl_audio_hooks.h"
#include "perf_monitor.h"
#include "switch_audio.h"

#include <switch.h>

#include <SDL2/SDL.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

// main.cpp: the host's SDL log function (writes host.log).
extern "C" void cmz_sdl_log_to_host(void* userdata, int category, SDL_LogPriority priority,
                                    const char* message);

namespace {

constexpr s32 kTimeCriticalPriority = 0x2A;
constexpr int kMaxDevices = 4;
constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

struct DeviceHook {
    std::atomic<bool> used;
    SDL_AudioCallback callback;
    void* userdata;
    SDL_AudioFormat format;
};

DeviceHook g_devices[kMaxDevices];

std::atomic<uint32_t> g_devices_opened{0};
std::atomic<uint32_t> g_open_failures{0};
std::atomic<uint64_t> g_callbacks{0};
std::atomic<uint64_t> g_callback_ticks{0};
std::atomic<uint64_t> g_silent_callbacks{0};
std::atomic<uint64_t> g_max_callback_ticks{0};
// Bits of a non-negative float; such floats order like their bit patterns.
std::atomic<uint32_t> g_peak_bits{0};
std::atomic<bool> g_priority_raised{false};

void note(const char* format, ...) __attribute__((format(printf, 1, 2)));

void note(const char* format, ...) {
    char text[224];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    cmz_perf_note(text);
}

// SDL log capture, installed at the first audio open and chained to the
// function that was installed before it.
SDL_LogOutputFunction g_previous_log = nullptr;
void* g_previous_log_userdata = nullptr;
std::atomic<bool> g_log_capture_installed{false};

const char* priority_name(SDL_LogPriority priority) {
    switch (priority) {
        case SDL_LOG_PRIORITY_VERBOSE: return "verbose";
        case SDL_LOG_PRIORITY_DEBUG: return "debug";
        case SDL_LOG_PRIORITY_INFO: return "info";
        case SDL_LOG_PRIORITY_WARN: return "warn";
        case SDL_LOG_PRIORITY_ERROR: return "error";
        case SDL_LOG_PRIORITY_CRITICAL: return "critical";
        default: return "?";
    }
}

void SDLCALL capture_log(void* /*userdata*/, int category, SDL_LogPriority priority,
                         const char* message) {
    note("sdl[%s/%d]: %s", priority_name(priority), category,
         message != nullptr ? message : "(null)");
    if (g_previous_log != nullptr) {
        g_previous_log(g_previous_log_userdata, category, priority, message);
    }
}

void install_log_capture() {
    bool expected = false;
    if (!g_log_capture_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    SDL_LogGetOutputFunction(&g_previous_log, &g_previous_log_userdata);
    if (g_previous_log == capture_log) {
        g_previous_log = nullptr;
    }
    if (g_previous_log != cmz_sdl_log_to_host) {
        note("sdl log output was not the host's at audio open (%p); messages are copied here",
             reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_previous_log)));
    }
    SDL_LogSetOutputFunction(capture_log, nullptr);
}

void store_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t previous = target.load(kRelaxed);
    while (value > previous && !target.compare_exchange_weak(previous, value, kRelaxed)) {
    }
}

void store_peak(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t previous = g_peak_bits.load(kRelaxed);
    while (bits > previous && !g_peak_bits.compare_exchange_weak(previous, bits, kRelaxed)) {
    }
}

// Highest absolute sample value in the buffer, or -1 for formats not measured.
float buffer_peak(SDL_AudioFormat format, const Uint8* stream, int length) {
    float peak = 0.0f;
    if (format == AUDIO_F32SYS) {
        const float* samples = reinterpret_cast<const float*>(stream);
        const int count = length / static_cast<int>(sizeof(float));
        for (int index = 0; index < count; ++index) {
            const float value = samples[index] < 0.0f ? -samples[index] : samples[index];
            if (value > peak) {
                peak = value;
            }
        }
        return peak;
    }
    if (format == AUDIO_S16SYS) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(stream);
        const int count = length / static_cast<int>(sizeof(int16_t));
        int highest = 0;
        for (int index = 0; index < count; ++index) {
            const int value = samples[index] < 0 ? -samples[index] : samples[index];
            if (value > highest) {
                highest = value;
            }
        }
        return static_cast<float>(highest) / 32768.0f;
    }
    return -1.0f;
}

void SDLCALL hooked_callback(void* userdata, Uint8* stream, int length) {
    DeviceHook* hook = static_cast<DeviceHook*>(userdata);
    const uint64_t start = armGetSystemTick();
    hook->callback(hook->userdata, stream, length);
    const uint64_t elapsed = armGetSystemTick() - start;

    g_callbacks.fetch_add(1, kRelaxed);
    g_callback_ticks.fetch_add(elapsed, kRelaxed);
    store_max(g_max_callback_ticks, elapsed);
    const float peak = buffer_peak(hook->format, stream, length);
    if (peak == 0.0f) {
        g_silent_callbacks.fetch_add(1, kRelaxed);
    } else if (peak > 0.0f) {
        store_peak(peak);
    }
}

} // namespace

extern "C" {

// SDL internal (src/thread/SDL_systhread.h); not in the public headers.
int __real_SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority);

int __wrap_SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority) {
    if (priority == SDL_THREAD_PRIORITY_TIME_CRITICAL) {
        if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, kTimeCriticalPriority))) {
            if (!g_priority_raised.exchange(true, kRelaxed)) {
                note("audio: SDL audio thread priority raised to 0x%X", kTimeCriticalPriority);
            }
            return 0;
        }
        // Fall back to SDL's own mapping for HIGH (0x2B).
        return __real_SDL_SYS_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    }
    return __real_SDL_SYS_SetThreadPriority(priority);
}

decltype(SDL_OpenAudioDevice) __real_SDL_OpenAudioDevice;

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* device, int iscapture,
                                             const SDL_AudioSpec* desired,
                                             SDL_AudioSpec* obtained, int allowed_changes) {
    install_log_capture();
    DeviceHook* hook = nullptr;
    SDL_AudioSpec hooked {};
    const SDL_AudioSpec* request = desired;
    if (desired != nullptr && desired->callback != nullptr && iscapture == 0) {
        for (DeviceHook& candidate : g_devices) {
            bool expected = false;
            if (candidate.used.compare_exchange_strong(expected, true)) {
                hook = &candidate;
                break;
            }
        }
    }
    if (hook != nullptr) {
        // The slot is complete before SDL can start the device thread.
        hook->callback = desired->callback;
        hook->userdata = desired->userdata;
        hook->format = desired->format;
        hooked = *desired;
        hooked.callback = hooked_callback;
        hooked.userdata = hook;
        request = &hooked;
    }

    const SDL_AudioDeviceID id =
        __real_SDL_OpenAudioDevice(device, iscapture, request, obtained, allowed_changes);
    if (id == 0) {
        g_open_failures.fetch_add(1, kRelaxed);
        note("audio: SDL_OpenAudioDevice(%s) failed: %s", device != nullptr ? device : "default",
             SDL_GetError());
        if (hook != nullptr) {
            hook->used.store(false, kRelaxed);
        }
        return 0;
    }

    g_devices_opened.fetch_add(1, kRelaxed);
    // With an obtained spec, SDL calls back in that format; without one, in
    // the desired format.
    const SDL_AudioSpec* used = obtained != nullptr ? obtained : desired;
    if (hook != nullptr && used != nullptr) {
        hook->format = used->format;
    }
    if (used != nullptr) {
        note("audio: device %u opened on driver %s: %d Hz, format 0x%04X, %u channels, %u samples%s",
             static_cast<unsigned>(id),
             SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "?",
             used->freq, static_cast<unsigned>(used->format),
             static_cast<unsigned>(used->channels), static_cast<unsigned>(used->samples),
             hook != nullptr ? ", callback measured" : "");
    }
    return id;
}

void cmz_audio_take_report(CmzAudioReport* report) {
    report->devices_opened = g_devices_opened.load(kRelaxed);
    report->open_failures = g_open_failures.load(kRelaxed);
    report->callbacks = g_callbacks.load(kRelaxed);
    report->callback_ticks = g_callback_ticks.load(kRelaxed);
    report->silent_callbacks = g_silent_callbacks.load(kRelaxed);
    report->max_callback_ticks = g_max_callback_ticks.exchange(0, kRelaxed);
    const uint32_t bits = g_peak_bits.exchange(0, kRelaxed);
    std::memcpy(&report->peak, &bits, sizeof(bits));
    report->priority_raised = g_priority_raised.load(kRelaxed) ? 1 : 0;
    CmzSwitchAudioStats backend {};
    cmz_switch_audio_stats(&backend);
    report->underruns = backend.underruns;
    report->voice_drops = backend.voice_drops;
    report->buffers = backend.buffers;
    report->buffer_samples = backend.buffer_samples;
}

} // extern "C"

// The public wrapper must have exactly SDL's type; a mismatch would pass
// corrupted arguments. SDL_SYS_SetThreadPriority has no public prototype; its
// signature is SDL 2.28.5's src/thread/SDL_systhread.h.
#define CMZ_CHECK_WRAPPER(name)                                                       \
    static_assert(std::is_same<decltype(__wrap_##name), decltype(name)>::value,       \
                  #name " wrapper must match SDL.h");                                 \
    static_assert(std::is_same<decltype(__real_##name), decltype(name)>::value,       \
                  #name " real declaration must match SDL.h")
#define CMZ_INTERNAL_WRAPPER(name)                                                    \
    static_assert(std::is_same<decltype(__wrap_##name), decltype(__real_##name)>::value, \
                  #name " wrapper and real declaration must match")

CMZ_CHECK_WRAPPER(SDL_OpenAudioDevice);
CMZ_INTERNAL_WRAPPER(SDL_SYS_SetThreadPriority);

#undef CMZ_CHECK_WRAPPER
#undef CMZ_INTERNAL_WRAPPER
