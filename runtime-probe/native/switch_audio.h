#pragma once

// castleminerZ-nx's Nintendo Switch audio backend for SDL (see switch_audio.cpp).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio buffers to keep queued in the audio renderer, 2-4. 0 keeps the default
// (4). Takes effect when SDL next opens the audio device.
void cmz_switch_audio_configure(int buffers);

typedef struct CmzSwitchAudioStats {
    // Configuration of the open device; 0 before the first open.
    int buffers;
    uint32_t buffer_samples;
    // Cumulative since start.
    uint64_t underruns;   // times every queued buffer had played before the next one arrived
    uint32_t voice_drops; // voice drops reported by the audio renderer
} CmzSwitchAudioStats;

void cmz_switch_audio_stats(CmzSwitchAudioStats* stats);

#ifdef __cplusplus
}
#endif
