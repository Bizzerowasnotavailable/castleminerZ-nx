// newlib only declares POSIX functions such as vsnprintf's siblings when a
// feature level is requested; the pinned mono-nx Makefile compiles C++ in
// strict -std=c++17 mode.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

// castleminerZ-nx performance monitor. See perf_monitor.h and
// docs/performance.md.
//
// Design rules:
//   * Nothing here may change what the game does. Every FNA3D wrapper calls the
//     real function with the same arguments and only reads the system tick
//     around it. The linker routes calls through the wrappers (--wrap), and the
//     static_asserts below pin each wrapper to the exact FNA3D.h prototype.
//   * The per-call path is cheap: two tick reads and a few plain adds to
//     counters that only the presenting (main) thread writes. That thread
//     publishes a copy once per frame; the report thread only reads.
//   * The report and sampler threads are plain libnx threads. They never call
//     into Mono and run off the main thread's core when the process allows it.
//   * The sampler pauses a thread only to read its registers and walk its frame
//     records. Between pause and resume nothing allocates, locks or does I/O.

#include "perf_monitor.h"
#include "sdl_audio_hooks.h"

#include <switch.h>

#include <mono/metadata/profiler.h>

#include <FNA3D.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

// The embedder defines the profiler state type; nothing is stored in it.
struct _MonoProfiler {
    int unused;
};

namespace {

constexpr const char* kPerfLogPath = "sdmc:/switch/castleminerz-logs/perf.log";
constexpr const char* kProfileConfigPath = "sdmc:/switch/castleminerz-logs/profile.ini";
constexpr const char* kProfileOutputPath = "sdmc:/switch/castleminerz-logs/profile.bin";

constexpr uint64_t kReportIntervalNs = 5000000000ull;
constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

// Priorities: lower numbers run first. The main thread runs at 0x2C. The
// sampler must preempt the threads it samples; the reporter must not matter.
constexpr int kSamplerPriority = 0x2A;
constexpr int kReporterPriority = 0x3B;

CmzPerfLogFn g_host_log = nullptr;
bool g_initialized = false;
std::atomic<bool> g_runtime_attached{false};
Handle g_main_thread = INVALID_HANDLE;
uint64_t g_main_stack_lo = 0;
uint64_t g_main_stack_hi = 0;
uint64_t g_module_base = 0;
uint64_t g_module_size = 0;
uint64_t g_tick_freq = 19200000;
uint64_t g_start_tick = 0;
u64 g_process_core_mask = 0;
// svcGetInfo id that returns a thread's CPU time, or 0 when none works.
u32 g_thread_tick_info = 0;
std::atomic<bool> g_shutting_down{false};

void host_logf(const char* format, ...) __attribute__((format(printf, 1, 2)));

void host_logf(const char* format, ...) {
    if (g_host_log == nullptr) {
        return;
    }
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_host_log(line);
}

double ticks_to_ms(uint64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_tick_freq);
}

// Bounds of the memory mapping that contains `address`.
bool mapping_bounds(uint64_t address, uint64_t* lo, uint64_t* hi) {
    MemoryInfo info {};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, address)) || info.size == 0) {
        return false;
    }
    *lo = info.addr;
    *hi = info.addr + info.size;
    return true;
}

bool thread_cpu_ticks(Handle thread, uint64_t* ticks) {
    if (g_thread_tick_info == 0 || thread == INVALID_HANDLE) {
        return false;
    }
    return R_SUCCEEDED(svcGetInfo(ticks, g_thread_tick_info, thread, UINT64_MAX));
}

// Firmware 13.0.0 replaced the thread CPU time info id; probe both once.
void detect_thread_tick_info() {
    uint64_t ticks = 0;
    if (R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, g_main_thread, UINT64_MAX))) {
        g_thread_tick_info = InfoType_ThreadTickCount;
    } else if (R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated,
                                      g_main_thread, UINT64_MAX))) {
        g_thread_tick_info = InfoType_ThreadTickCountDeprecated;
    }
}

void copy_name(char* destination, size_t size, const char* source) {
    size_t length = 0;
    for (; source[length] != '\0' && length + 1 < size; ++length) {
        const char c = source[length];
        // Keep perf.log one record per line and space-separated.
        destination[length] = (c == ' ' || c == '=' || c < 0x20) ? '_' : c;
    }
    destination[length] = '\0';
}

// ---------------------------------------------------------------------------
// FNA3D accounting

// "clear" is separate from "target": Mesa acquires the next swap-chain buffer at
// the first use of the window framebuffer in a frame, normally the first
// Clear, so the wait for vsync lands there.
enum GfxSlot : int {
    kGfxDraw = 0,
    kGfxEffect,
    kGfxState,
    kGfxSampler,
    kGfxUpload,
    kGfxTexture,
    kGfxClear,
    kGfxTarget,
    kGfxResource,
    kGfxReadback,
    kGfxPresent,
    kGfxSlotCount
};

constexpr const char* kGfxNames[kGfxSlotCount] = {
    "draw", "effect", "state", "sampler", "upload", "texture",
    "clear", "target", "resource", "readback", "present",
};

struct GfxTotals {
    uint64_t calls[kGfxSlotCount];
    uint64_t ticks[kGfxSlotCount];
    uint64_t primitives;
    uint64_t upload_bytes;
    uint64_t texture_bytes;
};

// Running totals of the presenting thread; only that thread writes them.
GfxTotals g_main_gfx;

struct PublishedGfx {
    std::atomic<uint64_t> calls[kGfxSlotCount];
    std::atomic<uint64_t> ticks[kGfxSlotCount];
    std::atomic<uint64_t> primitives;
    std::atomic<uint64_t> upload_bytes;
    std::atomic<uint64_t> texture_bytes;
};

// Copy published once per frame for the report thread.
PublishedGfx g_published_gfx;

// FNA3D calls made off the main thread (FNA3D forwards most of them to it).
std::atomic<uint64_t> g_offthread_calls{0};
std::atomic<uint64_t> g_offthread_ticks{0};

// Frame intervals (present end to present end): 0.25 ms bins below 50 ms,
// 1 ms bins below 250 ms, then one overflow bin. Cumulative counts written by
// the presenting thread only.
constexpr int kFrameBins = 401;
std::atomic<uint32_t> g_frame_bins[kFrameBins];
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_frame_interval_ticks{0};
// Longest interval since the last report; the report thread swaps it to 0.
std::atomic<uint64_t> g_frame_interval_max{0};
uint64_t g_last_present_end = 0;

int frame_bin(uint64_t ticks) {
    const uint64_t us = ticks * 1000000ull / g_tick_freq;
    if (us < 50000) {
        return static_cast<int>(us / 250);
    }
    if (us < 250000) {
        return 200 + static_cast<int>((us - 50000) / 1000);
    }
    return kFrameBins - 1;
}

double frame_bin_upper_ms(int bin) {
    if (bin < 200) {
        return (bin + 1) * 0.25;
    }
    if (bin < kFrameBins - 1) {
        return 50.0 + (bin - 200 + 1);
    }
    return 250.0;
}

void store_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t previous = target.load(kRelaxed);
    while (value > previous &&
           !target.compare_exchange_weak(previous, value, kRelaxed)) {
    }
}

inline bool on_main_thread() {
    return threadGetCurHandle() == g_main_thread;
}

inline void account(int slot, uint64_t start) {
    const uint64_t elapsed = armGetSystemTick() - start;
    if (on_main_thread()) {
        g_main_gfx.calls[slot] += 1;
        g_main_gfx.ticks[slot] += elapsed;
    } else {
        g_offthread_calls.fetch_add(1, kRelaxed);
        g_offthread_ticks.fetch_add(elapsed, kRelaxed);
    }
}

inline void account_draw(uint64_t start, int32_t primitives) {
    const uint64_t elapsed = armGetSystemTick() - start;
    if (on_main_thread()) {
        g_main_gfx.calls[kGfxDraw] += 1;
        g_main_gfx.ticks[kGfxDraw] += elapsed;
        if (primitives > 0) {
            g_main_gfx.primitives += static_cast<uint64_t>(primitives);
        }
    } else {
        g_offthread_calls.fetch_add(1, kRelaxed);
        g_offthread_ticks.fetch_add(elapsed, kRelaxed);
    }
}

inline void account_bytes(int slot, uint64_t start, int64_t bytes) {
    const uint64_t elapsed = armGetSystemTick() - start;
    if (on_main_thread()) {
        g_main_gfx.calls[slot] += 1;
        g_main_gfx.ticks[slot] += elapsed;
        if (bytes > 0) {
            if (slot == kGfxTexture) {
                g_main_gfx.texture_bytes += static_cast<uint64_t>(bytes);
            } else {
                g_main_gfx.upload_bytes += static_cast<uint64_t>(bytes);
            }
        }
    } else {
        g_offthread_calls.fetch_add(1, kRelaxed);
        g_offthread_ticks.fetch_add(elapsed, kRelaxed);
    }
}

void publish_gfx() {
    for (int slot = 0; slot < kGfxSlotCount; ++slot) {
        g_published_gfx.calls[slot].store(g_main_gfx.calls[slot], kRelaxed);
        g_published_gfx.ticks[slot].store(g_main_gfx.ticks[slot], kRelaxed);
    }
    g_published_gfx.primitives.store(g_main_gfx.primitives, kRelaxed);
    g_published_gfx.upload_bytes.store(g_main_gfx.upload_bytes, kRelaxed);
    g_published_gfx.texture_bytes.store(g_main_gfx.texture_bytes, kRelaxed);
}

void on_present(uint64_t start) {
    const uint64_t end = armGetSystemTick();
    if (!on_main_thread()) {
        g_offthread_calls.fetch_add(1, kRelaxed);
        g_offthread_ticks.fetch_add(end - start, kRelaxed);
        return;
    }
    g_main_gfx.calls[kGfxPresent] += 1;
    g_main_gfx.ticks[kGfxPresent] += end - start;
    if (g_last_present_end != 0) {
        const uint64_t interval = end - g_last_present_end;
        const int bin = frame_bin(interval);
        g_frame_bins[bin].store(g_frame_bins[bin].load(kRelaxed) + 1, kRelaxed);
        g_frame_interval_ticks.store(g_frame_interval_ticks.load(kRelaxed) + interval, kRelaxed);
        store_max(g_frame_interval_max, interval);
        g_frames.store(g_frames.load(kRelaxed) + 1, kRelaxed);
    }
    g_last_present_end = end;
    publish_gfx();
}

// ---------------------------------------------------------------------------
// Mono GC pauses and managed threads

std::atomic<uint64_t> g_gc_minor{0};
std::atomic<uint64_t> g_gc_major{0};
std::atomic<uint64_t> g_gc_pauses{0};
std::atomic<uint64_t> g_gc_pause_ticks{0};
std::atomic<uint64_t> g_gc_pause_max{0};
std::atomic<uint64_t> g_gc_stop_start{0};

void on_gc_event(MonoProfiler* /*profiler*/, MonoProfilerGCEvent event,
                 uint32_t generation, mono_bool /*is_serial*/) {
    switch (event) {
        case MONO_GC_EVENT_PRE_STOP_WORLD:
            g_gc_stop_start.store(armGetSystemTick(), kRelaxed);
            break;
        case MONO_GC_EVENT_START:
            (generation == 0 ? g_gc_minor : g_gc_major).fetch_add(1, kRelaxed);
            break;
        case MONO_GC_EVENT_POST_START_WORLD: {
            const uint64_t stop_start = g_gc_stop_start.exchange(0, kRelaxed);
            if (stop_start != 0) {
                const uint64_t pause = armGetSystemTick() - stop_start;
                g_gc_pauses.fetch_add(1, kRelaxed);
                g_gc_pause_ticks.fetch_add(pause, kRelaxed);
                store_max(g_gc_pause_max, pause);
            }
            break;
        }
        default:
            break;
    }
}

constexpr int kMaxThreads = 32;

struct ThreadSlot {
    bool used;
    bool alive;
    Handle handle;
    uintptr_t tid;
    uint64_t stack_lo;
    uint64_t stack_hi;
    uint32_t name_version;
    char name[32];
    // Owned by the report thread.
    uint64_t report_ticks;
    bool report_valid;
    // Owned by the sampler thread.
    uint32_t sampler_name_version;
};

// Guards g_threads. Taken by Mono profiler callbacks, the report thread and
// the sampler; never held while another thread is paused and then waited on.
Mutex g_threads_lock;
ThreadSlot g_threads[kMaxThreads];

void default_thread_name(char* destination, size_t size, int slot) {
    char digits[4];
    int count = 0;
    int value = slot + 1;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 3);
    size_t length = 0;
    for (const char* prefix = "thread"; *prefix != '\0' && length + 1 < size; ++prefix) {
        destination[length++] = *prefix;
    }
    while (count > 0 && length + 1 < size) {
        destination[length++] = digits[--count];
    }
    destination[length] = '\0';
}

void on_thread_started(MonoProfiler* /*profiler*/, uintptr_t tid) {
    const Handle handle = threadGetCurHandle();
    if (handle == g_main_thread) {
        return;
    }
    uint64_t lo = 0;
    uint64_t hi = 0;
    if (!mapping_bounds(reinterpret_cast<uint64_t>(__builtin_frame_address(0)), &lo, &hi)) {
        lo = hi = 0;
    }

    mutexLock(&g_threads_lock);
    int chosen = -1;
    for (int index = 0; index < kMaxThreads && chosen < 0; ++index) {
        if (!g_threads[index].used) {
            chosen = index;
        }
    }
    for (int index = 0; index < kMaxThreads && chosen < 0; ++index) {
        if (!g_threads[index].alive) {
            chosen = index;
        }
    }
    if (chosen >= 0) {
        ThreadSlot& slot = g_threads[chosen];
        slot.used = true;
        slot.alive = true;
        slot.handle = handle;
        slot.tid = tid;
        slot.stack_lo = lo;
        slot.stack_hi = hi;
        slot.report_valid = false;
        default_thread_name(slot.name, sizeof(slot.name), chosen);
        slot.name_version += 1;
    }
    mutexUnlock(&g_threads_lock);
}

void on_thread_name(MonoProfiler* /*profiler*/, uintptr_t tid, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    mutexLock(&g_threads_lock);
    for (ThreadSlot& slot : g_threads) {
        if (slot.alive && slot.tid == tid) {
            copy_name(slot.name, sizeof(slot.name), name);
            slot.name_version += 1;
            break;
        }
    }
    mutexUnlock(&g_threads_lock);
}

void on_thread_stopped(MonoProfiler* /*profiler*/, uintptr_t tid) {
    mutexLock(&g_threads_lock);
    for (ThreadSlot& slot : g_threads) {
        if (slot.alive && slot.tid == tid) {
            slot.alive = false;
            slot.handle = INVALID_HANDLE;
            break;
        }
    }
    mutexUnlock(&g_threads_lock);
}

// ---------------------------------------------------------------------------
// Notes: one-off lines (audio device details, captured SDL messages) that the
// report thread writes to perf.log. perf.log is the one log the report thread
// is known to keep writing once the game runs.

constexpr int kNoteSlots = 64;
constexpr size_t kNoteLength = 240;

struct NoteQueue {
    Mutex lock;
    uint32_t head;
    uint32_t count;
    uint32_t dropped;
    char text[kNoteSlots][kNoteLength];
};

NoteQueue g_notes;

void write_notes(std::FILE* log) {
    static char pending[kNoteSlots][kNoteLength];
    uint32_t count = 0;
    uint32_t dropped = 0;
    mutexLock(&g_notes.lock);
    for (; count < g_notes.count; ++count) {
        std::memcpy(pending[count], g_notes.text[(g_notes.head + count) % kNoteSlots], kNoteLength);
    }
    g_notes.head = (g_notes.head + count) % kNoteSlots;
    g_notes.count = 0;
    dropped = g_notes.dropped;
    g_notes.dropped = 0;
    mutexUnlock(&g_notes.lock);

    for (uint32_t index = 0; index < count; ++index) {
        std::fprintf(log, "# %s\n", pending[index]);
    }
    if (dropped > 0) {
        std::fprintf(log, "# (%u more notes dropped)\n", dropped);
    }
}

// ---------------------------------------------------------------------------
// Report thread

struct Snapshot {
    uint64_t tick;
    uint64_t frames;
    uint64_t frame_ticks;
    uint32_t bins[kFrameBins];
    uint64_t gfx_calls[kGfxSlotCount];
    uint64_t gfx_ticks[kGfxSlotCount];
    uint64_t primitives;
    uint64_t upload_bytes;
    uint64_t texture_bytes;
    uint64_t offthread_calls;
    uint64_t offthread_ticks;
    uint64_t gc_minor;
    uint64_t gc_major;
    uint64_t gc_pauses;
    uint64_t gc_pause_ticks;
    uint64_t main_cpu_ticks;
    bool main_cpu_valid;
};

void take_snapshot(Snapshot* snapshot) {
    snapshot->tick = armGetSystemTick();
    snapshot->frames = g_frames.load(kRelaxed);
    snapshot->frame_ticks = g_frame_interval_ticks.load(kRelaxed);
    for (int bin = 0; bin < kFrameBins; ++bin) {
        snapshot->bins[bin] = g_frame_bins[bin].load(kRelaxed);
    }
    for (int slot = 0; slot < kGfxSlotCount; ++slot) {
        snapshot->gfx_calls[slot] = g_published_gfx.calls[slot].load(kRelaxed);
        snapshot->gfx_ticks[slot] = g_published_gfx.ticks[slot].load(kRelaxed);
    }
    snapshot->primitives = g_published_gfx.primitives.load(kRelaxed);
    snapshot->upload_bytes = g_published_gfx.upload_bytes.load(kRelaxed);
    snapshot->texture_bytes = g_published_gfx.texture_bytes.load(kRelaxed);
    snapshot->offthread_calls = g_offthread_calls.load(kRelaxed);
    snapshot->offthread_ticks = g_offthread_ticks.load(kRelaxed);
    snapshot->gc_minor = g_gc_minor.load(kRelaxed);
    snapshot->gc_major = g_gc_major.load(kRelaxed);
    snapshot->gc_pauses = g_gc_pauses.load(kRelaxed);
    snapshot->gc_pause_ticks = g_gc_pause_ticks.load(kRelaxed);
    snapshot->main_cpu_valid = thread_cpu_ticks(g_main_thread, &snapshot->main_cpu_ticks);
}

struct LineBuilder {
    char text[3072];
    size_t length = 0;

    void add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (length + 1 >= sizeof(text)) {
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(text + length, sizeof(text) - length, format, args);
        va_end(args);
        if (written > 0) {
            length += static_cast<size_t>(written);
            if (length >= sizeof(text)) {
                length = sizeof(text) - 1;
            }
        }
    }
};

const char* operation_mode_name() {
    return appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld";
}

double percentile_ms(const uint32_t* counts, uint64_t total, double fraction) {
    if (total == 0) {
        return 0.0;
    }
    const uint64_t target = static_cast<uint64_t>(fraction * static_cast<double>(total) + 0.5);
    uint64_t seen = 0;
    for (int bin = 0; bin < kFrameBins; ++bin) {
        seen += counts[bin];
        if (seen >= target && seen > 0) {
            return frame_bin_upper_ms(bin);
        }
    }
    return frame_bin_upper_ms(kFrameBins - 1);
}

void write_report(std::FILE* log, const Snapshot& before, const Snapshot& after) {
    static LineBuilder line;
    static uint32_t counts[kFrameBins];
    line.length = 0;

    const uint64_t wall = after.tick - before.tick;
    const double seconds = ticks_to_ms(wall) / 1000.0;
    const uint64_t frames = after.frames - before.frames;
    line.add("t=%.1f mode=%s fps=%.2f", ticks_to_ms(after.tick - g_start_tick) / 1000.0,
             operation_mode_name(), seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0);

    const uint64_t max_interval = g_frame_interval_max.exchange(0, kRelaxed);
    if (frames > 0) {
        uint64_t total = 0;
        uint64_t over_33 = 0;
        for (int bin = 0; bin < kFrameBins; ++bin) {
            counts[bin] = after.bins[bin] - before.bins[bin];
            total += counts[bin];
            if (frame_bin_upper_ms(bin) > 33.5) {
                over_33 += counts[bin];
            }
        }
        const double per_frame = 1.0 / static_cast<double>(frames);
        line.add(" frame_ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f over33=%llu",
                 ticks_to_ms(after.frame_ticks - before.frame_ticks) * per_frame,
                 percentile_ms(counts, total, 0.50), percentile_ms(counts, total, 0.95),
                 percentile_ms(counts, total, 0.99), ticks_to_ms(max_interval),
                 static_cast<unsigned long long>(over_33));

        uint64_t gfx_ticks = 0;
        for (int slot = 0; slot < kGfxSlotCount; ++slot) {
            if (slot != kGfxPresent) {
                gfx_ticks += after.gfx_ticks[slot] - before.gfx_ticks[slot];
            }
        }
        line.add(" | gfx_ms=%.2f present_ms=%.2f",
                 ticks_to_ms(gfx_ticks) * per_frame,
                 ticks_to_ms(after.gfx_ticks[kGfxPresent] - before.gfx_ticks[kGfxPresent]) * per_frame);
        for (int slot = 0; slot < kGfxSlotCount; ++slot) {
            if (slot == kGfxPresent) {
                continue;
            }
            const uint64_t calls = after.gfx_calls[slot] - before.gfx_calls[slot];
            const uint64_t ticks = after.gfx_ticks[slot] - before.gfx_ticks[slot];
            line.add(" %s=%.2f/%.1f", kGfxNames[slot], ticks_to_ms(ticks) * per_frame,
                     static_cast<double>(calls) * per_frame);
        }
        line.add(" prims=%.0f upload_kb=%.1f texture_kb=%.1f",
                 static_cast<double>(after.primitives - before.primitives) * per_frame,
                 static_cast<double>(after.upload_bytes - before.upload_bytes) * per_frame / 1024.0,
                 static_cast<double>(after.texture_bytes - before.texture_bytes) * per_frame / 1024.0);
    }
    const uint64_t offthread = after.offthread_calls - before.offthread_calls;
    if (offthread > 0) {
        line.add(" offthread=%llu/%.2fms", static_cast<unsigned long long>(offthread),
                 ticks_to_ms(after.offthread_ticks - before.offthread_ticks));
    }

    if (before.main_cpu_valid && after.main_cpu_valid && wall > 0) {
        line.add(" | main_cpu=%.0f%%",
                 100.0 * static_cast<double>(after.main_cpu_ticks - before.main_cpu_ticks) /
                     static_cast<double>(wall));
    }

    const bool runtime_attached = g_runtime_attached.load(kRelaxed);
    if (runtime_attached) {
        line.add(" | gc minor=%llu major=%llu pause_ms=%.2f max=%.2f",
                 static_cast<unsigned long long>(after.gc_minor - before.gc_minor),
                 static_cast<unsigned long long>(after.gc_major - before.gc_major),
                 ticks_to_ms(after.gc_pause_ticks - before.gc_pause_ticks),
                 ticks_to_ms(g_gc_pause_max.exchange(0, kRelaxed)));
    }

    // Audio device callbacks (FAudio's mixer) and the level they produced.
    static CmzAudioReport previous_audio;
    CmzAudioReport audio;
    cmz_audio_take_report(&audio);
    if (audio.devices_opened == 0 && audio.open_failures > 0) {
        line.add(" | audio open-failed=%u", audio.open_failures);
    } else if (audio.devices_opened == 0) {
        line.add(" | audio closed");
    } else {
        const uint64_t callbacks = audio.callbacks - previous_audio.callbacks;
        line.add(" | audio cb=%llu mix_ms avg=%.2f max=%.2f peak=%.3f silent_cb=%llu prio=%s",
                 static_cast<unsigned long long>(callbacks),
                 callbacks > 0 ? ticks_to_ms(audio.callback_ticks - previous_audio.callback_ticks) /
                                     static_cast<double>(callbacks)
                               : 0.0,
                 ticks_to_ms(audio.max_callback_ticks), static_cast<double>(audio.peak),
                 static_cast<unsigned long long>(audio.silent_callbacks -
                                                 previous_audio.silent_callbacks),
                 audio.priority_raised ? "raised" : "sdl");
    }
    previous_audio = audio;

    // Managed threads that used at least 1% of a core.
    bool header = false;
    int alive = 0;
    mutexLock(&g_threads_lock);
    for (ThreadSlot& slot : g_threads) {
        if (!slot.alive) {
            continue;
        }
        ++alive;
        uint64_t ticks = 0;
        if (!thread_cpu_ticks(slot.handle, &ticks)) {
            slot.report_valid = false;
            continue;
        }
        if (slot.report_valid && wall > 0) {
            const double percent = 100.0 * static_cast<double>(ticks - slot.report_ticks) /
                                   static_cast<double>(wall);
            if (percent >= 1.0) {
                if (!header) {
                    line.add(" | threads");
                    header = true;
                }
                line.add(" %s=%.0f%%", slot.name, percent);
            }
        }
        slot.report_ticks = ticks;
        slot.report_valid = true;
    }
    mutexUnlock(&g_threads_lock);
    if (runtime_attached) {
        line.add(" managed_threads=%d", alive);
    }

    line.add("\n");
    std::fputs(line.text, log);
    std::fflush(log);
}

void write_header(std::FILE* log) {
    const u32 version = hosversionGet();
    std::fprintf(log,
                 "# castleminerZ-nx perf log v1: firmware %u.%u.%u, %s, process core mask 0x%llx, "
                 "thread cpu time %s, report every %llu s\n",
                 HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
                 operation_mode_name(), static_cast<unsigned long long>(g_process_core_mask),
                 g_thread_tick_info != 0 ? "available" : "unavailable",
                 static_cast<unsigned long long>(kReportIntervalNs / 1000000000ull));
    std::fputs("# frame_ms: present-to-present interval. gfx_ms: main-thread time inside FNA3D "
               "calls per frame, excluding present. <category>=<ms per frame>/<calls per frame>; "
               "clear includes the wait for the next swap-chain buffer (vsync). present_ms: time "
               "inside FNA3D_SwapBuffers. main_cpu: CPU time of the main thread over wall time. "
               "gc pause_ms: stop-the-world time in the interval. audio: device callbacks in the "
               "interval, mixing time per callback, highest output sample, callbacks that output "
               "only silence, audio thread priority.\n",
               log);
    std::fflush(log);
}

Thread g_reporter_thread;

void reporter_main(void* /*argument*/) {
    std::FILE* log = std::fopen(kPerfLogPath, "w");
    if (log == nullptr) {
        host_logf("perf: cannot create %s", kPerfLogPath);
        return;
    }
    write_header(log);

    static Snapshot snapshots[2];
    int current = 0;
    take_snapshot(&snapshots[current]);
    while (!g_shutting_down.load(kRelaxed)) {
        svcSleepThread(kReportIntervalNs);
        if (g_shutting_down.load(kRelaxed)) {
            break;
        }
        const int next = current ^ 1;
        take_snapshot(&snapshots[next]);
        write_notes(log);
        write_report(log, snapshots[current], snapshots[next]);
        current = next;
    }
    std::fclose(log);
}

// ---------------------------------------------------------------------------
// Sampling profiler (opt-in)

struct ProfileSettings {
    bool enabled = false;
    uint32_t hz = 500;
    bool all_threads = false;
    uint32_t delay_s = 0;
    uint32_t duration_s = 0;
};

ProfileSettings g_profile;

uint32_t parse_bounded(const char* value, uint32_t minimum, uint32_t maximum) {
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    if (parsed < minimum) {
        return minimum;
    }
    if (parsed > maximum) {
        return maximum;
    }
    return static_cast<uint32_t>(parsed);
}

void load_profile_settings() {
    std::FILE* file = std::fopen(kProfileConfigPath, "r");
    if (file == nullptr) {
        return;
    }
    g_profile.enabled = true;
    char text[160];
    while (std::fgets(text, sizeof(text), file) != nullptr) {
        char key[32];
        char value[64];
        if (std::sscanf(text, " %31[^=; \t\r\n] = %63s", key, value) != 2) {
            continue;
        }
        if (std::strcmp(key, "enabled") == 0) {
            g_profile.enabled = std::strcmp(value, "false") != 0 && std::strcmp(value, "0") != 0;
        } else if (std::strcmp(key, "hz") == 0) {
            g_profile.hz = parse_bounded(value, 50, 2000);
        } else if (std::strcmp(key, "threads") == 0) {
            g_profile.all_threads = std::strcmp(value, "all") == 0;
        } else if (std::strcmp(key, "delay") == 0) {
            g_profile.delay_s = parse_bounded(value, 0, 3600);
        } else if (std::strcmp(key, "duration") == 0) {
            g_profile.duration_s = parse_bounded(value, 0, 86400);
        }
    }
    std::fclose(file);
}

// profile.bin: ProfileFileHeader, then records of little-endian 32-bit words.
// Record word 0 = kind | slot << 8 | payload_words << 16.
//   kind 1, sample: tick (low 32 bits), pc, lr, return addresses (frame chain)
//   kind 2, thread name: 8 words of NUL-padded name
// Addresses are offsets from the module base (ELF addresses, the ELF is
// linked at 0); 0xFFFFFFFF marks an address outside the module.
struct ProfileFileHeader {
    char magic[8];
    uint32_t version;
    uint32_t hz;
    uint64_t module_base;
    uint64_t module_size;
    uint64_t tick_freq;
    uint64_t start_tick;
};

constexpr uint32_t kRecordSample = 1;
constexpr uint32_t kRecordThreadName = 2;
constexpr int kMaxStackDepth = 48;
constexpr size_t kSampleBufferWords = 256 * 1024;
// Room for one round: the main thread and every managed thread.
constexpr size_t kRoundWords = (kMaxThreads + 1) * (1 + 3 + kMaxStackDepth + 1 + 8);

uint32_t module_offset(uint64_t address) {
    if (address >= g_module_base && address - g_module_base < g_module_size &&
        address - g_module_base < 0xFFFFFFFFull) {
        return static_cast<uint32_t>(address - g_module_base);
    }
    return 0xFFFFFFFFu;
}

// Returns the number of frame-chain entries, or -1 if no sample was taken.
int capture_stack(Handle thread, uint64_t lo, uint64_t hi,
                  uint64_t* pc, uint64_t* lr, uint64_t* frames) {
    if (R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused))) {
        return -1;
    }
    ThreadContext context;
    const Result rc = svcGetThreadContext3(&context, thread);
    int depth = 0;
    if (R_SUCCEEDED(rc)) {
        *pc = context.pc.x;
        *lr = context.lr;
        uint64_t fp = context.fp;
        while (depth < kMaxStackDepth && hi - lo >= 16 && fp >= lo && fp <= hi - 16 &&
               (fp & 7) == 0) {
            const volatile uint64_t* record = reinterpret_cast<const volatile uint64_t*>(fp);
            const uint64_t next = record[0];
            frames[depth++] = record[1];
            if (next <= fp) {
                break;
            }
            fp = next;
        }
    }
    svcSetThreadActivity(thread, ThreadActivity_Runnable);
    return R_SUCCEEDED(rc) ? depth : -1;
}

struct SampleBuffer {
    uint32_t* words = nullptr;
    size_t used = 0;
};

void append_name(SampleBuffer& buffer, uint32_t slot, const char* name) {
    uint32_t* out = buffer.words + buffer.used;
    out[0] = kRecordThreadName | (slot << 8) | (8u << 16);
    char padded[32] = {};
    copy_name(padded, sizeof(padded), name);
    std::memcpy(out + 1, padded, sizeof(padded));
    buffer.used += 9;
}

bool append_sample(SampleBuffer& buffer, uint32_t slot, Handle thread, uint64_t lo, uint64_t hi) {
    uint64_t pc = 0;
    uint64_t lr = 0;
    uint64_t frames[kMaxStackDepth];
    const int depth = capture_stack(thread, lo, hi, &pc, &lr, frames);
    if (depth < 0) {
        return false;
    }
    uint32_t* out = buffer.words + buffer.used;
    const uint32_t payload = 3 + static_cast<uint32_t>(depth);
    out[0] = kRecordSample | (slot << 8) | (payload << 16);
    out[1] = static_cast<uint32_t>(armGetSystemTick());
    out[2] = module_offset(pc);
    out[3] = module_offset(lr);
    for (int index = 0; index < depth; ++index) {
        out[4 + index] = module_offset(frames[index]);
    }
    buffer.used += 1 + payload;
    return true;
}

Thread g_sampler_thread;

void sampler_main(void* /*argument*/) {
    std::FILE* out = std::fopen(kProfileOutputPath, "wb");
    if (out == nullptr) {
        host_logf("profiler: cannot create %s", kProfileOutputPath);
        return;
    }
    SampleBuffer buffer;
    buffer.words = static_cast<uint32_t*>(std::malloc(kSampleBufferWords * sizeof(uint32_t)));
    if (buffer.words == nullptr) {
        host_logf("profiler: out of memory");
        std::fclose(out);
        return;
    }

    if (g_profile.delay_s > 0) {
        svcSleepThread(static_cast<uint64_t>(g_profile.delay_s) * 1000000000ull);
    }
    const uint64_t start = armGetSystemTick();
    ProfileFileHeader header {};
    std::memcpy(header.magic, "CMZPROF", 8);
    header.version = 1;
    header.hz = g_profile.hz;
    header.module_base = g_module_base;
    header.module_size = g_module_size;
    header.tick_freq = g_tick_freq;
    header.start_tick = start;
    std::fwrite(&header, sizeof(header), 1, out);
    append_name(buffer, 0, "main");

    const uint64_t period_ns = 1000000000ull / g_profile.hz;
    const uint64_t end = g_profile.duration_s > 0
                             ? start + static_cast<uint64_t>(g_profile.duration_s) * g_tick_freq
                             : UINT64_MAX;
    uint64_t last_flush = start;
    uint64_t round = 0;
    uint64_t taken = 0;
    uint64_t failed = 0;
    host_logf("profiler: sampling %s at %u Hz into %s",
              g_profile.all_threads ? "all managed threads" : "the main thread",
              g_profile.hz, kProfileOutputPath);

    while (!g_shutting_down.load(kRelaxed)) {
        svcSleepThread(period_ns);
        const uint64_t now = armGetSystemTick();
        if (now >= end) {
            break;
        }

        if (append_sample(buffer, 0, g_main_thread, g_main_stack_lo, g_main_stack_hi)) {
            ++taken;
        } else {
            ++failed;
        }

        // Managed threads at a quarter of the rate. The lock is taken only
        // after the main thread has been resumed.
        if (g_profile.all_threads && (round & 3) == 0) {
            mutexLock(&g_threads_lock);
            for (int index = 0; index < kMaxThreads; ++index) {
                ThreadSlot& slot = g_threads[index];
                if (!slot.alive || slot.stack_hi <= slot.stack_lo) {
                    continue;
                }
                const uint32_t record_slot = static_cast<uint32_t>(index + 1);
                if (slot.sampler_name_version != slot.name_version) {
                    append_name(buffer, record_slot, slot.name);
                    slot.sampler_name_version = slot.name_version;
                }
                if (append_sample(buffer, record_slot, slot.handle, slot.stack_lo, slot.stack_hi)) {
                    ++taken;
                } else {
                    ++failed;
                }
            }
            mutexUnlock(&g_threads_lock);
        }
        ++round;

        if (buffer.used + kRoundWords > kSampleBufferWords || now - last_flush >= g_tick_freq) {
            std::fwrite(buffer.words, sizeof(uint32_t), buffer.used, out);
            std::fflush(out);
            buffer.used = 0;
            last_flush = now;
        }
    }

    std::fwrite(buffer.words, sizeof(uint32_t), buffer.used, out);
    std::fclose(out);
    std::free(buffer.words);
    host_logf("profiler: stopped, %llu samples, %llu failed",
              static_cast<unsigned long long>(taken), static_cast<unsigned long long>(failed));
}

bool start_thread(Thread* thread, ThreadFunc entry, int priority, size_t stack_size) {
    Result rc = threadCreate(thread, entry, nullptr, nullptr, stack_size, priority, -2);
    if (R_FAILED(rc)) {
        host_logf("perf: threadCreate(priority 0x%x) failed: 0x%x", priority, rc);
        return false;
    }
    // Keep these threads off core 0, where the game's main thread runs, when
    // the process may use cores 1 and 2; otherwise allow any permitted core.
    u64 mask = g_process_core_mask & 0x6;
    if (mask == 0) {
        mask = g_process_core_mask;
    }
    if (mask != 0) {
        rc = svcSetThreadCoreMask(thread->handle, -1, static_cast<u32>(mask));
        if (R_FAILED(rc)) {
            host_logf("perf: svcSetThreadCoreMask(0x%llx) failed: 0x%x",
                      static_cast<unsigned long long>(mask), rc);
        }
    }
    rc = threadStart(thread);
    if (R_FAILED(rc)) {
        host_logf("perf: threadStart failed: 0x%x", rc);
        threadClose(thread);
        return false;
    }
    return true;
}

void mark_shutting_down() {
    g_shutting_down.store(true, kRelaxed);
}

MonoProfiler g_profiler_state;

} // namespace

// ---------------------------------------------------------------------------
// FNA3D wrappers. The linker resolves every reference to FNA3D_X to
// __wrap_FNA3D_X and __real_FNA3D_X to the original (see build-game.ps1,
// which derives the --wrap list from the __wrap_ definitions in this file).

extern "C" {

decltype(FNA3D_SwapBuffers) __real_FNA3D_SwapBuffers;
decltype(FNA3D_Clear) __real_FNA3D_Clear;
decltype(FNA3D_DrawIndexedPrimitives) __real_FNA3D_DrawIndexedPrimitives;
decltype(FNA3D_DrawInstancedPrimitives) __real_FNA3D_DrawInstancedPrimitives;
decltype(FNA3D_DrawPrimitives) __real_FNA3D_DrawPrimitives;
decltype(FNA3D_SetViewport) __real_FNA3D_SetViewport;
decltype(FNA3D_SetScissorRect) __real_FNA3D_SetScissorRect;
decltype(FNA3D_SetBlendFactor) __real_FNA3D_SetBlendFactor;
decltype(FNA3D_SetMultiSampleMask) __real_FNA3D_SetMultiSampleMask;
decltype(FNA3D_SetReferenceStencil) __real_FNA3D_SetReferenceStencil;
decltype(FNA3D_SetBlendState) __real_FNA3D_SetBlendState;
decltype(FNA3D_SetDepthStencilState) __real_FNA3D_SetDepthStencilState;
decltype(FNA3D_ApplyRasterizerState) __real_FNA3D_ApplyRasterizerState;
decltype(FNA3D_VerifySampler) __real_FNA3D_VerifySampler;
decltype(FNA3D_VerifyVertexSampler) __real_FNA3D_VerifyVertexSampler;
decltype(FNA3D_ApplyVertexBufferBindings) __real_FNA3D_ApplyVertexBufferBindings;
decltype(FNA3D_SetRenderTargets) __real_FNA3D_SetRenderTargets;
decltype(FNA3D_ResolveTarget) __real_FNA3D_ResolveTarget;
decltype(FNA3D_GenVertexBuffer) __real_FNA3D_GenVertexBuffer;
decltype(FNA3D_AddDisposeVertexBuffer) __real_FNA3D_AddDisposeVertexBuffer;
decltype(FNA3D_SetVertexBufferData) __real_FNA3D_SetVertexBufferData;
decltype(FNA3D_GetVertexBufferData) __real_FNA3D_GetVertexBufferData;
decltype(FNA3D_GenIndexBuffer) __real_FNA3D_GenIndexBuffer;
decltype(FNA3D_AddDisposeIndexBuffer) __real_FNA3D_AddDisposeIndexBuffer;
decltype(FNA3D_SetIndexBufferData) __real_FNA3D_SetIndexBufferData;
decltype(FNA3D_GetIndexBufferData) __real_FNA3D_GetIndexBufferData;
decltype(FNA3D_CreateTexture2D) __real_FNA3D_CreateTexture2D;
decltype(FNA3D_SetTextureData2D) __real_FNA3D_SetTextureData2D;
decltype(FNA3D_GetTextureData2D) __real_FNA3D_GetTextureData2D;
decltype(FNA3D_ReadBackbuffer) __real_FNA3D_ReadBackbuffer;
decltype(FNA3D_SetEffectTechnique) __real_FNA3D_SetEffectTechnique;
decltype(FNA3D_ApplyEffect) __real_FNA3D_ApplyEffect;
decltype(FNA3D_BeginPassRestore) __real_FNA3D_BeginPassRestore;
decltype(FNA3D_EndPassRestore) __real_FNA3D_EndPassRestore;

void __wrap_FNA3D_SwapBuffers(FNA3D_Device* device, FNA3D_Rect* sourceRectangle,
                              FNA3D_Rect* destinationRectangle, void* overrideWindowHandle) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SwapBuffers(device, sourceRectangle, destinationRectangle, overrideWindowHandle);
    on_present(start);
}

void __wrap_FNA3D_Clear(FNA3D_Device* device, FNA3D_ClearOptions options, FNA3D_Vec4* color,
                        float depth, int32_t stencil) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_Clear(device, options, color, depth, stencil);
    account(kGfxClear, start);
}

void __wrap_FNA3D_DrawIndexedPrimitives(FNA3D_Device* device, FNA3D_PrimitiveType primitiveType,
                                        int32_t baseVertex, int32_t minVertexIndex,
                                        int32_t numVertices, int32_t startIndex,
                                        int32_t primitiveCount, FNA3D_Buffer* indices,
                                        FNA3D_IndexElementSize indexElementSize) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_DrawIndexedPrimitives(device, primitiveType, baseVertex, minVertexIndex,
                                       numVertices, startIndex, primitiveCount, indices,
                                       indexElementSize);
    account_draw(start, primitiveCount);
}

void __wrap_FNA3D_DrawInstancedPrimitives(FNA3D_Device* device, FNA3D_PrimitiveType primitiveType,
                                          int32_t baseVertex, int32_t minVertexIndex,
                                          int32_t numVertices, int32_t startIndex,
                                          int32_t primitiveCount, int32_t instanceCount,
                                          FNA3D_Buffer* indices,
                                          FNA3D_IndexElementSize indexElementSize) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_DrawInstancedPrimitives(device, primitiveType, baseVertex, minVertexIndex,
                                         numVertices, startIndex, primitiveCount, instanceCount,
                                         indices, indexElementSize);
    account_draw(start, primitiveCount);
}

void __wrap_FNA3D_DrawPrimitives(FNA3D_Device* device, FNA3D_PrimitiveType primitiveType,
                                 int32_t vertexStart, int32_t primitiveCount) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_DrawPrimitives(device, primitiveType, vertexStart, primitiveCount);
    account_draw(start, primitiveCount);
}

void __wrap_FNA3D_SetViewport(FNA3D_Device* device, FNA3D_Viewport* viewport) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetViewport(device, viewport);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetScissorRect(FNA3D_Device* device, FNA3D_Rect* scissor) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetScissorRect(device, scissor);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetBlendFactor(FNA3D_Device* device, FNA3D_Color* blendFactor) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetBlendFactor(device, blendFactor);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetMultiSampleMask(FNA3D_Device* device, int32_t mask) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetMultiSampleMask(device, mask);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetReferenceStencil(FNA3D_Device* device, int32_t ref) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetReferenceStencil(device, ref);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetBlendState(FNA3D_Device* device, FNA3D_BlendState* blendState) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetBlendState(device, blendState);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetDepthStencilState(FNA3D_Device* device,
                                       FNA3D_DepthStencilState* depthStencilState) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetDepthStencilState(device, depthStencilState);
    account(kGfxState, start);
}

void __wrap_FNA3D_ApplyRasterizerState(FNA3D_Device* device,
                                       FNA3D_RasterizerState* rasterizerState) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_ApplyRasterizerState(device, rasterizerState);
    account(kGfxState, start);
}

void __wrap_FNA3D_VerifySampler(FNA3D_Device* device, int32_t index, FNA3D_Texture* texture,
                                FNA3D_SamplerState* sampler) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_VerifySampler(device, index, texture, sampler);
    account(kGfxSampler, start);
}

void __wrap_FNA3D_VerifyVertexSampler(FNA3D_Device* device, int32_t index, FNA3D_Texture* texture,
                                      FNA3D_SamplerState* sampler) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_VerifyVertexSampler(device, index, texture, sampler);
    account(kGfxSampler, start);
}

void __wrap_FNA3D_ApplyVertexBufferBindings(FNA3D_Device* device,
                                            FNA3D_VertexBufferBinding* bindings,
                                            int32_t numBindings, uint8_t bindingsUpdated,
                                            int32_t baseVertex) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_ApplyVertexBufferBindings(device, bindings, numBindings, bindingsUpdated,
                                           baseVertex);
    account(kGfxState, start);
}

void __wrap_FNA3D_SetRenderTargets(FNA3D_Device* device, FNA3D_RenderTargetBinding* renderTargets,
                                   int32_t numRenderTargets, FNA3D_Renderbuffer* depthStencilBuffer,
                                   FNA3D_DepthFormat depthFormat, uint8_t preserveTargetContents) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetRenderTargets(device, renderTargets, numRenderTargets, depthStencilBuffer,
                                  depthFormat, preserveTargetContents);
    account(kGfxTarget, start);
}

void __wrap_FNA3D_ResolveTarget(FNA3D_Device* device, FNA3D_RenderTargetBinding* target) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_ResolveTarget(device, target);
    account(kGfxTarget, start);
}

FNA3D_Buffer* __wrap_FNA3D_GenVertexBuffer(FNA3D_Device* device, uint8_t dynamic,
                                           FNA3D_BufferUsage usage, int32_t sizeInBytes) {
    const uint64_t start = armGetSystemTick();
    FNA3D_Buffer* result = __real_FNA3D_GenVertexBuffer(device, dynamic, usage, sizeInBytes);
    account(kGfxResource, start);
    return result;
}

void __wrap_FNA3D_AddDisposeVertexBuffer(FNA3D_Device* device, FNA3D_Buffer* buffer) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_AddDisposeVertexBuffer(device, buffer);
    account(kGfxResource, start);
}

void __wrap_FNA3D_SetVertexBufferData(FNA3D_Device* device, FNA3D_Buffer* buffer,
                                      int32_t offsetInBytes, void* data, int32_t elementCount,
                                      int32_t elementSizeInBytes, int32_t vertexStride,
                                      FNA3D_SetDataOptions options) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetVertexBufferData(device, buffer, offsetInBytes, data, elementCount,
                                     elementSizeInBytes, vertexStride, options);
    account_bytes(kGfxUpload, start,
                  static_cast<int64_t>(elementCount) * static_cast<int64_t>(elementSizeInBytes));
}

void __wrap_FNA3D_GetVertexBufferData(FNA3D_Device* device, FNA3D_Buffer* buffer,
                                      int32_t offsetInBytes, void* data, int32_t elementCount,
                                      int32_t elementSizeInBytes, int32_t vertexStride) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_GetVertexBufferData(device, buffer, offsetInBytes, data, elementCount,
                                     elementSizeInBytes, vertexStride);
    account(kGfxReadback, start);
}

FNA3D_Buffer* __wrap_FNA3D_GenIndexBuffer(FNA3D_Device* device, uint8_t dynamic,
                                          FNA3D_BufferUsage usage, int32_t sizeInBytes) {
    const uint64_t start = armGetSystemTick();
    FNA3D_Buffer* result = __real_FNA3D_GenIndexBuffer(device, dynamic, usage, sizeInBytes);
    account(kGfxResource, start);
    return result;
}

void __wrap_FNA3D_AddDisposeIndexBuffer(FNA3D_Device* device, FNA3D_Buffer* buffer) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_AddDisposeIndexBuffer(device, buffer);
    account(kGfxResource, start);
}

void __wrap_FNA3D_SetIndexBufferData(FNA3D_Device* device, FNA3D_Buffer* buffer,
                                     int32_t offsetInBytes, void* data, int32_t dataLength,
                                     FNA3D_SetDataOptions options) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetIndexBufferData(device, buffer, offsetInBytes, data, dataLength, options);
    account_bytes(kGfxUpload, start, dataLength);
}

void __wrap_FNA3D_GetIndexBufferData(FNA3D_Device* device, FNA3D_Buffer* buffer,
                                     int32_t offsetInBytes, void* data, int32_t dataLength) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_GetIndexBufferData(device, buffer, offsetInBytes, data, dataLength);
    account(kGfxReadback, start);
}

FNA3D_Texture* __wrap_FNA3D_CreateTexture2D(FNA3D_Device* device, FNA3D_SurfaceFormat format,
                                            int32_t width, int32_t height, int32_t levelCount,
                                            uint8_t isRenderTarget) {
    const uint64_t start = armGetSystemTick();
    FNA3D_Texture* result = __real_FNA3D_CreateTexture2D(device, format, width, height,
                                                         levelCount, isRenderTarget);
    account(kGfxResource, start);
    return result;
}

void __wrap_FNA3D_SetTextureData2D(FNA3D_Device* device, FNA3D_Texture* texture, int32_t x,
                                   int32_t y, int32_t w, int32_t h, int32_t level, void* data,
                                   int32_t dataLength) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetTextureData2D(device, texture, x, y, w, h, level, data, dataLength);
    account_bytes(kGfxTexture, start, dataLength);
}

void __wrap_FNA3D_GetTextureData2D(FNA3D_Device* device, FNA3D_Texture* texture, int32_t x,
                                   int32_t y, int32_t w, int32_t h, int32_t level, void* data,
                                   int32_t dataLength) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_GetTextureData2D(device, texture, x, y, w, h, level, data, dataLength);
    account(kGfxReadback, start);
}

void __wrap_FNA3D_ReadBackbuffer(FNA3D_Device* device, int32_t x, int32_t y, int32_t w, int32_t h,
                                 void* data, int32_t dataLength) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_ReadBackbuffer(device, x, y, w, h, data, dataLength);
    account(kGfxReadback, start);
}

void __wrap_FNA3D_SetEffectTechnique(FNA3D_Device* device, FNA3D_Effect* effect,
                                     MOJOSHADER_effectTechnique* technique) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_SetEffectTechnique(device, effect, technique);
    account(kGfxEffect, start);
}

void __wrap_FNA3D_ApplyEffect(FNA3D_Device* device, FNA3D_Effect* effect, uint32_t pass,
                              MOJOSHADER_effectStateChanges* stateChanges) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_ApplyEffect(device, effect, pass, stateChanges);
    account(kGfxEffect, start);
}

void __wrap_FNA3D_BeginPassRestore(FNA3D_Device* device, FNA3D_Effect* effect,
                                   MOJOSHADER_effectStateChanges* stateChanges) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_BeginPassRestore(device, effect, stateChanges);
    account(kGfxEffect, start);
}

void __wrap_FNA3D_EndPassRestore(FNA3D_Device* device, FNA3D_Effect* effect) {
    const uint64_t start = armGetSystemTick();
    __real_FNA3D_EndPassRestore(device, effect);
    account(kGfxEffect, start);
}

} // extern "C"

// Each wrapper and each __real_ declaration must have exactly the FNA3D.h
// type; a mismatch would pass corrupted arguments at runtime.
#define CMZ_CHECK_WRAPPER(name)                                                       \
    static_assert(std::is_same<decltype(__wrap_##name), decltype(name)>::value,       \
                  #name " wrapper must match FNA3D.h");                               \
    static_assert(std::is_same<decltype(__real_##name), decltype(name)>::value,       \
                  #name " real declaration must match FNA3D.h")

CMZ_CHECK_WRAPPER(FNA3D_SwapBuffers);
CMZ_CHECK_WRAPPER(FNA3D_Clear);
CMZ_CHECK_WRAPPER(FNA3D_DrawIndexedPrimitives);
CMZ_CHECK_WRAPPER(FNA3D_DrawInstancedPrimitives);
CMZ_CHECK_WRAPPER(FNA3D_DrawPrimitives);
CMZ_CHECK_WRAPPER(FNA3D_SetViewport);
CMZ_CHECK_WRAPPER(FNA3D_SetScissorRect);
CMZ_CHECK_WRAPPER(FNA3D_SetBlendFactor);
CMZ_CHECK_WRAPPER(FNA3D_SetMultiSampleMask);
CMZ_CHECK_WRAPPER(FNA3D_SetReferenceStencil);
CMZ_CHECK_WRAPPER(FNA3D_SetBlendState);
CMZ_CHECK_WRAPPER(FNA3D_SetDepthStencilState);
CMZ_CHECK_WRAPPER(FNA3D_ApplyRasterizerState);
CMZ_CHECK_WRAPPER(FNA3D_VerifySampler);
CMZ_CHECK_WRAPPER(FNA3D_VerifyVertexSampler);
CMZ_CHECK_WRAPPER(FNA3D_ApplyVertexBufferBindings);
CMZ_CHECK_WRAPPER(FNA3D_SetRenderTargets);
CMZ_CHECK_WRAPPER(FNA3D_ResolveTarget);
CMZ_CHECK_WRAPPER(FNA3D_GenVertexBuffer);
CMZ_CHECK_WRAPPER(FNA3D_AddDisposeVertexBuffer);
CMZ_CHECK_WRAPPER(FNA3D_SetVertexBufferData);
CMZ_CHECK_WRAPPER(FNA3D_GetVertexBufferData);
CMZ_CHECK_WRAPPER(FNA3D_GenIndexBuffer);
CMZ_CHECK_WRAPPER(FNA3D_AddDisposeIndexBuffer);
CMZ_CHECK_WRAPPER(FNA3D_SetIndexBufferData);
CMZ_CHECK_WRAPPER(FNA3D_GetIndexBufferData);
CMZ_CHECK_WRAPPER(FNA3D_CreateTexture2D);
CMZ_CHECK_WRAPPER(FNA3D_SetTextureData2D);
CMZ_CHECK_WRAPPER(FNA3D_GetTextureData2D);
CMZ_CHECK_WRAPPER(FNA3D_ReadBackbuffer);
CMZ_CHECK_WRAPPER(FNA3D_SetEffectTechnique);
CMZ_CHECK_WRAPPER(FNA3D_ApplyEffect);
CMZ_CHECK_WRAPPER(FNA3D_BeginPassRestore);
CMZ_CHECK_WRAPPER(FNA3D_EndPassRestore);

#undef CMZ_CHECK_WRAPPER

// ---------------------------------------------------------------------------
// Entry points

extern "C" void cmz_perf_initialize(CmzPerfLogFn host_log) {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    g_host_log = host_log;
    g_tick_freq = armGetSystemTickFreq();
    g_start_tick = armGetSystemTick();
    g_main_thread = envGetMainThreadHandle();
    if (threadGetCurHandle() != g_main_thread) {
        host_logf("perf: warning: not called on the main thread");
    }

    if (!mapping_bounds(reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
                        &g_main_stack_lo, &g_main_stack_hi)) {
        g_main_stack_lo = g_main_stack_hi = 0;
    }
    uint64_t text_lo = 0;
    uint64_t text_hi = 0;
    if (mapping_bounds(reinterpret_cast<uint64_t>(&cmz_perf_initialize), &text_lo, &text_hi)) {
        g_module_base = text_lo;
        g_module_size = text_hi - text_lo;
    }
    if (R_FAILED(svcGetInfo(&g_process_core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        g_process_core_mask = 0;
    }
    detect_thread_tick_info();
    std::atexit(mark_shutting_down);

    host_logf("perf: text 0x%llx+0x%llx, main stack 0x%llx-0x%llx, core mask 0x%llx, "
              "thread cpu time %s",
              static_cast<unsigned long long>(g_module_base),
              static_cast<unsigned long long>(g_module_size),
              static_cast<unsigned long long>(g_main_stack_lo),
              static_cast<unsigned long long>(g_main_stack_hi),
              static_cast<unsigned long long>(g_process_core_mask),
              g_thread_tick_info == InfoType_ThreadTickCount
                  ? "info 25"
                  : (g_thread_tick_info != 0 ? "info 0xF0000002" : "unavailable"));

    if (!start_thread(&g_reporter_thread, reporter_main, kReporterPriority, 0x10000)) {
        host_logf("perf: report thread not started");
    }

    load_profile_settings();
    if (!g_profile.enabled) {
        return;
    }
    if (g_module_size == 0 || g_main_stack_hi <= g_main_stack_lo) {
        host_logf("profiler: disabled, module or stack bounds unknown");
        return;
    }
    if (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33)) {
        host_logf("profiler: disabled, svcSetThreadActivity/svcGetThreadContext3 unavailable");
        return;
    }
    if (!start_thread(&g_sampler_thread, sampler_main, kSamplerPriority, 0x10000) &&
        !start_thread(&g_sampler_thread, sampler_main, 0x2C, 0x10000)) {
        host_logf("profiler: thread not started");
    }
}

extern "C" void cmz_perf_note(const char* text) {
    if (text == nullptr) {
        return;
    }
    mutexLock(&g_notes.lock);
    if (g_notes.count < static_cast<uint32_t>(kNoteSlots)) {
        char* slot = g_notes.text[(g_notes.head + g_notes.count) % kNoteSlots];
        // One record per line in perf.log.
        size_t length = 0;
        for (; text[length] != '\0' && length + 1 < kNoteLength; ++length) {
            slot[length] = (text[length] == '\n' || text[length] == '\r') ? ' ' : text[length];
        }
        while (length > 0 && slot[length - 1] == ' ') {
            --length;
        }
        slot[length] = '\0';
        g_notes.count += 1;
    } else {
        g_notes.dropped += 1;
    }
    mutexUnlock(&g_notes.lock);
}

extern "C" void cmz_perf_attach_runtime(void) {
    if (!g_initialized || g_runtime_attached.load(kRelaxed)) {
        return;
    }
    MonoProfilerHandle handle = mono_profiler_create(&g_profiler_state);
    if (handle == nullptr) {
        host_logf("perf: mono_profiler_create failed");
        return;
    }
    mono_profiler_set_gc_event_callback(handle, on_gc_event);
    mono_profiler_set_thread_started_callback(handle, on_thread_started);
    mono_profiler_set_thread_name_callback(handle, on_thread_name);
    mono_profiler_set_thread_stopped_callback(handle, on_thread_stopped);
    mono_profiler_set_thread_exited_callback(handle, on_thread_stopped);
    g_runtime_attached.store(true, kRelaxed);
    host_logf("perf: Mono GC and thread callbacks installed");
}
