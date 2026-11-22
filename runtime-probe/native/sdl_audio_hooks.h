#pragma once

// SDL audio hooks for the Switch build (see sdl_audio_hooks.cpp): the audio
// thread priority fix and audio output statistics for perf.log.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CmzAudioReport {
    // Cumulative since start.
    uint32_t devices_opened;
    uint32_t open_failures;
    uint64_t callbacks;
    uint64_t callback_ticks;
    uint64_t silent_callbacks;
    // Since the previous cmz_audio_take_report call.
    uint64_t max_callback_ticks;
    float peak;
    // Whether SDL's audio thread runs at the raised priority.
    int priority_raised;
} CmzAudioReport;

// Fills *report; the "since the previous call" fields are reset.
void cmz_audio_take_report(CmzAudioReport* report);

#ifdef __cplusplus
}
#endif
