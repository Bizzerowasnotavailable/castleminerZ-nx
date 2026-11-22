// castleminerZ-nx: Nintendo Switch audio backend for SDL 2.28.5.
//
// SDL's own backend (src/audio/switch/SDL_switchaudio.c in devkitPro's SDL)
// keeps two wave buffers. After queuing one it waits until that buffer starts
// playing, and only then lets the mixer produce the next. It also refreshes the
// renderer state before waiting for the next renderer frame rather than after,
// so it notices each change 5-10 ms late. On hardware the device therefore ran
// dry between buffers: with 480-sample (10 ms) buffers the mixer ran about 85
// times a second for a device that plays 100 buffers a second. 1024-sample
// buffers (0.5.1) left less of a gap, but the audio was still choppy.
//
// This backend keeps up to four buffers queued (the audio renderer's limit for
// one voice). It refills the oldest one as soon as the renderer reports that it
// has played. A slow mixer callback or a late wake-up then costs latency, not a
// gap. It replaces SDL's at link time: build-game.ps1 passes
// -Wl,--wrap=SWITCHAUDIO_bootstrap, so SDL's driver table points at
// __wrap_SWITCHAUDIO_bootstrap below and SDL's own backend is not linked.
//
// SDL drives a backend through its private structures (src/audio/SDL_sysaudio.h).
// They are declared below for SDL 2.28.5 only. The static_asserts pin the
// offsets that SDL's own SWITCHAUDIO_* code in the linked libSDL2.a uses
// (read from its disassembly): spec at 8, hidden at 144, and the driver entry
// points at 8/16/32/40/48/72 with OnlyHasDefaultOutputDevice at 128.

#include "switch_audio.h"
#include "perf_monitor.h"

#include <switch.h>

#include <SDL2/SDL.h>

#include <malloc.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

static_assert(SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL) ==
                  SDL_VERSIONNUM(2, 28, 5),
              "switch_audio.cpp mirrors SDL 2.28.5's private audio structures");

struct CmzSwitchAudioData;

// src/audio/SDL_sysaudio.h: struct SDL_AudioDevice.
struct CmzSdlAudioDevice {
    SDL_AudioDeviceID id;
    SDL_AudioSpec spec;
    SDL_AudioSpec callbackspec;
    SDL_AudioStream* stream;
    SDL_atomic_t shutdown;
    SDL_atomic_t enabled;
    SDL_atomic_t paused;
    SDL_bool iscapture;
    Uint8* work_buffer;
    Uint32 work_buffer_len;
    SDL_mutex* mixer_lock;
    SDL_Thread* thread;
    SDL_threadID threadid;
    void* buffer_queue; // SDL_DataQueue*
    CmzSwitchAudioData* hidden; // the backend's private data
    void* handle;
};

// src/audio/SDL_sysaudio.h: struct SDL_AudioDriverImpl.
struct CmzSdlAudioDriverImpl {
    void (*DetectDevices)(void);
    int (*OpenDevice)(CmzSdlAudioDevice* device, const char* devname);
    void (*ThreadInit)(CmzSdlAudioDevice* device);
    void (*ThreadDeinit)(CmzSdlAudioDevice* device);
    void (*WaitDevice)(CmzSdlAudioDevice* device);
    void (*PlayDevice)(CmzSdlAudioDevice* device);
    Uint8* (*GetDeviceBuf)(CmzSdlAudioDevice* device);
    int (*CaptureFromDevice)(CmzSdlAudioDevice* device, void* buffer, int buflen);
    void (*FlushCapture)(CmzSdlAudioDevice* device);
    void (*CloseDevice)(CmzSdlAudioDevice* device);
    void (*LockDevice)(CmzSdlAudioDevice* device);
    void (*UnlockDevice)(CmzSdlAudioDevice* device);
    void (*FreeDeviceHandle)(void* handle);
    void (*Deinitialize)(void);
    int (*GetDefaultAudioInfo)(char** name, SDL_AudioSpec* spec, int iscapture);
    SDL_bool ProvidesOwnCallbackThread;
    SDL_bool HasCaptureSupport;
    SDL_bool OnlyHasDefaultOutputDevice;
    SDL_bool OnlyHasDefaultCaptureDevice;
    SDL_bool AllowsArbitraryDeviceNames;
    SDL_bool SupportsNonPow2Samples;
};

// src/audio/SDL_sysaudio.h: struct AudioBootStrap.
struct CmzSdlAudioBootStrap {
    const char* name;
    const char* desc;
    SDL_bool (*init)(CmzSdlAudioDriverImpl* impl);
    SDL_bool demand_only;
};

static_assert(offsetof(SDL_AudioSpec, format) == 4 && offsetof(SDL_AudioSpec, samples) == 8 &&
                  offsetof(SDL_AudioSpec, size) == 12 && sizeof(SDL_AudioSpec) == 32,
              "SDL_AudioSpec layout");
static_assert(offsetof(CmzSdlAudioDevice, spec) == 8, "SDL_AudioDevice.spec");
static_assert(offsetof(CmzSdlAudioDevice, hidden) == 144, "SDL_AudioDevice.hidden");
static_assert(offsetof(CmzSdlAudioDriverImpl, OpenDevice) == 8, "impl.OpenDevice");
static_assert(offsetof(CmzSdlAudioDriverImpl, ThreadInit) == 16, "impl.ThreadInit");
static_assert(offsetof(CmzSdlAudioDriverImpl, WaitDevice) == 32, "impl.WaitDevice");
static_assert(offsetof(CmzSdlAudioDriverImpl, PlayDevice) == 40, "impl.PlayDevice");
static_assert(offsetof(CmzSdlAudioDriverImpl, GetDeviceBuf) == 48, "impl.GetDeviceBuf");
static_assert(offsetof(CmzSdlAudioDriverImpl, CloseDevice) == 72, "impl.CloseDevice");
static_assert(offsetof(CmzSdlAudioDriverImpl, OnlyHasDefaultOutputDevice) == 128,
              "impl.OnlyHasDefaultOutputDevice");
static_assert(sizeof(CmzSdlAudioBootStrap) == 32, "AudioBootStrap size");

namespace {

// The audio renderer plays at most four wave buffers of one voice at a time.
constexpr int kMaxBuffers = 4;
constexpr int kDefaultBuffers = 4;
constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

} // namespace

struct CmzSwitchAudioData {
    AudioDriver driver;
    bool renderer_initialized;
    bool driver_created;
    void* pool;
    size_t pool_size;
    int buffer_count;
    int next;        // buffer the mixer fills next: the oldest one in flight
    bool queued_any; // at least one buffer has been queued
    AudioDriverWaveBuf buffers[kMaxBuffers];
};

namespace {

std::atomic<int> g_requested_buffers{kDefaultBuffers};
std::atomic<int> g_open_buffers{0};
std::atomic<uint32_t> g_open_buffer_samples{0};
std::atomic<uint64_t> g_underruns{0};
std::atomic<uint32_t> g_voice_drops{0};

void note(const char* format, ...) __attribute__((format(printf, 1, 2)));

void note(const char* format, ...) {
    char text[224];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    cmz_perf_note(text);
}

AudioRendererConfig renderer_config() {
    // As SDL's backend: one 48 kHz stereo sink and room for 24 voices.
    AudioRendererConfig config {};
    config.output_rate = AudioRendererOutputRate_48kHz;
    config.num_voices = 24;
    config.num_effects = 0;
    config.num_sinks = 1;
    config.num_mix_objs = 1;
    config.num_mix_buffers = 2;
    return config;
}

bool buffer_free(const AudioDriverWaveBuf& buffer) {
    return buffer.state == AudioDriverWaveBufState_Free ||
           buffer.state == AudioDriverWaveBufState_Done;
}

Uint8* buffer_data(CmzSdlAudioDevice* device, int index) {
    return static_cast<Uint8*>(device->hidden->pool) +
           static_cast<size_t>(index) * device->spec.size;
}

int open_device(CmzSdlAudioDevice* device, const char* /*devname*/) {
    CmzSwitchAudioData* data = static_cast<CmzSwitchAudioData*>(SDL_calloc(1, sizeof(CmzSwitchAudioData)));
    if (data == nullptr) {
        return SDL_OutOfMemory();
    }
    // From here on SDL calls close_device on failure, which releases what exists.
    device->hidden = data;

    int buffers = g_requested_buffers.load(kRelaxed);
    if (buffers < 2 || buffers > kMaxBuffers) {
        buffers = kDefaultBuffers;
    }
    data->buffer_count = buffers;

    const AudioRendererConfig config = renderer_config();
    Result rc = audrenInitialize(&config);
    if (R_FAILED(rc)) {
        return SDL_SetError("audrenInitialize failed (0x%x)", rc);
    }
    data->renderer_initialized = true;

    rc = audrvCreate(&data->driver, &config, 2);
    if (R_FAILED(rc)) {
        return SDL_SetError("audrvCreate failed (0x%x)", rc);
    }
    data->driver_created = true;

    // Renderer voices take 16-bit PCM in mono or stereo. SDL converts the
    // callback's format (FAudio mixes 32-bit float) to this one.
    device->spec.format = AUDIO_S16SYS;
    if (device->spec.channels > 2) {
        device->spec.channels = 2;
    }
    device->spec.silence = 0;
    device->spec.size = static_cast<Uint32>(device->spec.samples) * device->spec.channels *
                        static_cast<Uint32>(sizeof(int16_t));

    const size_t bytes = device->spec.size;
    data->pool_size = (bytes * static_cast<size_t>(buffers) + 0xfff) & ~static_cast<size_t>(0xfff);
    data->pool = memalign(0x1000, data->pool_size);
    if (data->pool == nullptr) {
        return SDL_OutOfMemory();
    }
    std::memset(data->pool, 0, data->pool_size);
    armDCacheFlush(data->pool, data->pool_size);

    // As in SDL's backend, every wave buffer points at the whole pool and
    // selects its slice by sample offset, which keeps any buffer size aligned.
    for (int index = 0; index < buffers; ++index) {
        AudioDriverWaveBuf& buffer = data->buffers[index];
        buffer.data_raw = data->pool;
        buffer.size = bytes * static_cast<size_t>(buffers);
        buffer.start_sample_offset = index * device->spec.samples;
        buffer.end_sample_offset = buffer.start_sample_offset + device->spec.samples;
    }

    const int pool_id = audrvMemPoolAdd(&data->driver, data->pool, data->pool_size);
    if (pool_id < 0 || !audrvMemPoolAttach(&data->driver, pool_id)) {
        return SDL_SetError("audio memory pool setup failed");
    }

    static const u8 sink_channels[] = {0, 1};
    if (audrvDeviceSinkAdd(&data->driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels) < 0) {
        return SDL_SetError("audrvDeviceSinkAdd failed");
    }

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        return SDL_SetError("audrenStartAudioRenderer failed (0x%x)", rc);
    }

    if (!audrvVoiceInit(&data->driver, 0, device->spec.channels, PcmFormat_Int16,
                        device->spec.freq)) {
        return SDL_SetError("audrvVoiceInit failed");
    }
    audrvVoiceSetDestinationMix(&data->driver, 0, AUDREN_FINAL_MIX_ID);
    if (device->spec.channels == 1) {
        audrvVoiceSetMixFactor(&data->driver, 0, 1.0f, 0, 0);
        audrvVoiceSetMixFactor(&data->driver, 0, 1.0f, 0, 1);
    } else {
        audrvVoiceSetMixFactor(&data->driver, 0, 1.0f, 0, 0);
        audrvVoiceSetMixFactor(&data->driver, 0, 0.0f, 0, 1);
        audrvVoiceSetMixFactor(&data->driver, 0, 0.0f, 1, 0);
        audrvVoiceSetMixFactor(&data->driver, 0, 1.0f, 1, 1);
    }
    audrvVoiceStart(&data->driver, 0);

    g_open_buffers.store(buffers, kRelaxed);
    g_open_buffer_samples.store(device->spec.samples, kRelaxed);
    note("audio: castleminerZ-nx switch backend, %d x %u-sample buffers queued (%.1f ms), %d Hz, %u channels",
         buffers, static_cast<unsigned>(device->spec.samples),
         1000.0 * buffers * device->spec.samples / (device->spec.freq > 0 ? device->spec.freq : 48000),
         device->spec.freq, static_cast<unsigned>(device->spec.channels));
    return 0;
}

// The mixer writes straight into the next free slice of the renderer's pool.
Uint8* get_device_buf(CmzSdlAudioDevice* device) {
    return buffer_data(device, device->hidden->next);
}

void play_device(CmzSdlAudioDevice* device) {
    CmzSwitchAudioData* data = device->hidden;
    AudioDriverWaveBuf* buffer = &data->buffers[data->next];

    // wait_device refreshed the renderer state just now. A voice with nothing
    // left to play has run dry: the renderer played silence since the last
    // buffer ended, which is a gap the listener hears.
    if (data->queued_any && !audrvVoiceIsPlaying(&data->driver, 0)) {
        g_underruns.fetch_add(1, kRelaxed);
    }

    armDCacheFlush(buffer_data(device, data->next), device->spec.size);
    if (!audrvVoiceAddWaveBuf(&data->driver, 0, buffer)) {
        // Only possible for a buffer still in flight, which wait_device rules out.
        return;
    }
    audrvUpdate(&data->driver);
    data->queued_any = true;
    data->next = (data->next + 1) % data->buffer_count;
    g_voice_drops.store(audrvVoiceGetVoiceDropsCount(&data->driver, 0), kRelaxed);
}

// Returns as soon as the oldest queued buffer has played; buffers finish in
// the order they were queued. The renderer state is refreshed before each
// check, so a finished buffer is seen at the first renderer frame after it
// ends instead of one frame later.
void wait_device(CmzSdlAudioDevice* device) {
    CmzSwitchAudioData* data = device->hidden;
    const AudioDriverWaveBuf& buffer = data->buffers[data->next];
    for (;;) {
        audrvUpdate(&data->driver);
        if (buffer_free(buffer) || SDL_AtomicGet(&device->shutdown) != 0) {
            return;
        }
        audrenWaitFrame();
    }
}

void close_device(CmzSdlAudioDevice* device) {
    CmzSwitchAudioData* data = device->hidden;
    if (data == nullptr) {
        return;
    }
    if (data->driver_created) {
        audrvClose(&data->driver);
    }
    if (data->renderer_initialized) {
        audrenExit();
    }
    // The renderer has stopped reading the pool once audrenExit returns.
    std::free(data->pool);
    SDL_free(data);
    device->hidden = nullptr;
    g_open_buffers.store(0, kRelaxed);
}

void thread_init(CmzSdlAudioDevice* /*device*/) {
}

SDL_bool init_driver(CmzSdlAudioDriverImpl* impl) {
    impl->OpenDevice = open_device;
    impl->PlayDevice = play_device;
    impl->WaitDevice = wait_device;
    impl->GetDeviceBuf = get_device_buf;
    impl->CloseDevice = close_device;
    impl->ThreadInit = thread_init;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    return SDL_TRUE;
}

} // namespace

extern "C" {

// SDL's driver table refers to SWITCHAUDIO_bootstrap; the linker resolves that
// reference to this definition (-Wl,--wrap=SWITCHAUDIO_bootstrap). The name
// stays "switch", so SDL_GetCurrentAudioDriver() reports the same driver.
CmzSdlAudioBootStrap __wrap_SWITCHAUDIO_bootstrap = {
    "switch", "Nintendo Switch audio driver (castleminerZ-nx, queued buffers)", init_driver,
    SDL_FALSE
};

// SDL's original, declared for the type check below and never referenced, so
// its object file is not linked.
extern CmzSdlAudioBootStrap __real_SWITCHAUDIO_bootstrap;

void cmz_switch_audio_configure(int buffers) {
    g_requested_buffers.store(buffers >= 2 && buffers <= kMaxBuffers ? buffers : kDefaultBuffers,
                              kRelaxed);
}

void cmz_switch_audio_stats(CmzSwitchAudioStats* stats) {
    stats->buffers = g_open_buffers.load(kRelaxed);
    stats->buffer_samples = g_open_buffer_samples.load(kRelaxed);
    stats->underruns = g_underruns.load(kRelaxed);
    stats->voice_drops = g_voice_drops.load(kRelaxed);
}

} // extern "C"

// A data wrapper replaces a variable rather than a function. build-game.ps1
// collects the --wrap list from the __wrap_ definitions and requires one of
// these checks for each.
#define CMZ_DATA_WRAPPER(name)                                                        \
    static_assert(std::is_same<decltype(__wrap_##name), decltype(__real_##name)>::value, \
                  #name " wrapper and real declaration must match")

CMZ_DATA_WRAPPER(SWITCHAUDIO_bootstrap);

#undef CMZ_DATA_WRAPPER
