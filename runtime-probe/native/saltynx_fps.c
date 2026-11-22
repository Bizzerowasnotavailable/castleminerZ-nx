// castleminerZ-nx: frame rate for SaltyNX tools (Status Monitor's FPS counter).
//
// For retail games, SaltyNX's NX-FPS plugin runs inside the game process. It
// hooks the game's present calls and writes the frame rate into a block of
// SaltyNX's shared memory, where Status Monitor reads it. SaltyNX does not
// inject itself into a homebrew process in title-replacement mode ("hbloader"),
// and this NRO links Mesa statically, so nothing would measure it. Instead the
// host publishes the same block itself:
//
//  - A background thread connects to SaltyNX's "SaltySD" port and reserves a
//    block (command 6) and gets the shared-memory handle (command 7). Unlike
//    SaltyNX's other commands, these two are not restricted to injected
//    processes. Then it ends the session (command 0), as Status Monitor does.
//    Without SaltyNX the port does not exist and the thread exits; SaltyNX
//    serves one session at a time, so the game itself never waits on it.
//  - Every present updates the block as NX-FPS's NX_FPS_Math::PostFrame does:
//    FPS (frames in the last second), FPSavg, the last ten frame times in
//    system ticks, frameNumber and pluginActive.
//
// Status Monitor uses the first block with the NX-FPS magic. SaltyNX clears its
// shared memory (except the first word) when an application starts, and only
// one application runs at a time. A block from an earlier run of this NRO in
// the same hbloader process can survive, so any other block with the magic is
// withdrawn first. If SaltyNX has no space left (it only resets its allocator
// when it injects into a retail game), the block goes to offset 4, NX-FPS's
// usual place, which is free because nothing is injected into this process.
//
// Layout: SaltyNX saltysd_core/source/NX-FPS-Common.hpp (310 bytes).
// Status Monitor reads the first 174 bytes.

#include "saltynx_fps.h"
#include "perf_monitor.h"

#include <switch.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint16_t width;
    uint16_t height;
    uint16_t calls;
} CmzNxFpsResolution;

typedef struct __attribute__((packed)) {
    uint32_t MAGIC;
    uint8_t FPS;
    float FPSavg;
    bool pluginActive;
    uint8_t FPSlocked;
    uint8_t FPSmode;
    uint8_t ZeroSync;
    uint8_t patchApplied;
    uint8_t API; // 1 NVN, 2 EGL, 3 Vulkan
    uint32_t FPSticks[10];
    uint8_t Buffers;
    uint8_t SetBuffers;
    uint8_t ActiveBuffers;
    uint8_t SetActiveBuffers;
    uint8_t displaySync;
    CmzNxFpsResolution renderCalls[8];
    CmzNxFpsResolution viewportCalls[8];
    bool forceOriginalRefreshRate;
    bool dontForce60InDocked;
    bool forceSuspend;
    uint8_t currentRefreshRate;
    float readSpeedPerSecond;
    uint8_t FPSlockedDocked;
    uint64_t frameNumber;
    int8_t expectedSetBuffers;
    uint8_t PerfCounters[136]; // NVN performance counters; unused for EGL
} CmzNxFpsBlock;

_Static_assert(sizeof(CmzNxFpsBlock) == 310, "NX-FPS block size (SaltyNX)");
_Static_assert(offsetof(CmzNxFpsBlock, FPSavg) == 5, "FPSavg offset");
_Static_assert(offsetof(CmzNxFpsBlock, pluginActive) == 9, "pluginActive offset");
_Static_assert(offsetof(CmzNxFpsBlock, API) == 14, "API offset");
_Static_assert(offsetof(CmzNxFpsBlock, FPSticks) == 15, "FPSticks offset");
_Static_assert(offsetof(CmzNxFpsBlock, frameNumber) == 165, "frameNumber offset");
_Static_assert(offsetof(CmzNxFpsBlock, expectedSetBuffers) == 173, "Status Monitor reads 174 bytes");

#define CMZ_NXFPS_MAGIC 0x465053u
#define CMZ_NXFPS_API_EGL 2
#define CMZ_SALTYNX_SHMEM_SIZE 0x1000
#define CMZ_SALTYNX_FALLBACK_OFFSET 4
#define CMZ_TICKS_PER_SECOND 19200000ull

enum {
    kSaltyEndSession = 0,
    kSaltyReserveSharedMemory = 6,
    kSaltyGetSharedMemoryHandle = 7,
};

static _Atomic(CmzNxFpsBlock*) g_block = NULL;
static SharedMemory g_shared_memory;
static Thread g_thread;

// Present-thread state, as NX_FPS_Math in SaltyNX.
static uint64_t g_frame_end;
static uint64_t g_frame_avg;
static uint64_t g_second_start;
static uint32_t g_second_frames;
static uint32_t g_tick_index;

static void note(const char* format, ...) __attribute__((format(printf, 1, 2)));

static void note(const char* format, ...) {
    char text[200];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    cmz_perf_note(text);
}

static void connect_and_publish(void* argument) {
    (void) argument;

    Handle session = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&session, "SaltySD");
    if (R_FAILED(rc)) {
        note("saltynx: not running (0x%X); no frame-rate block", rc);
        return;
    }
    Service service;
    serviceCreate(&service, session);

    struct {
        u64 size;
        u64 reserved;
    } reserve_in = {sizeof(CmzNxFpsBlock), 0};
    struct {
        u64 offset;
        u64 reserved;
    } reserve_out = {0, 0};
    const Result reserve_rc = serviceDispatchInOut(&service, kSaltyReserveSharedMemory,
                                                   reserve_in, reserve_out,
                                                   .in_send_pid = true);

    Handle memory = INVALID_HANDLE;
    const Result handle_rc = serviceDispatch(&service, kSaltyGetSharedMemoryHandle,
        .in_send_pid = true,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &memory);

    // SaltyNX ends the session without replying; the result is ignored.
    serviceDispatch(&service, kSaltyEndSession, .in_send_pid = true);
    serviceClose(&service);

    if (R_FAILED(handle_rc)) {
        note("saltynx: shared memory handle request failed (0x%X)", handle_rc);
        return;
    }
    shmemLoadRemote(&g_shared_memory, memory, CMZ_SALTYNX_SHMEM_SIZE, Perm_Rw);
    rc = shmemMap(&g_shared_memory);
    if (R_FAILED(rc)) {
        note("saltynx: mapping shared memory failed (0x%X)", rc);
        shmemClose(&g_shared_memory);
        return;
    }

    size_t offset = CMZ_SALTYNX_FALLBACK_OFFSET;
    const bool reserved = R_SUCCEEDED(reserve_rc) && reserve_out.offset % 4 == 0 &&
                          reserve_out.offset + sizeof(CmzNxFpsBlock) <= CMZ_SALTYNX_SHMEM_SIZE;
    if (reserved) {
        offset = (size_t) reserve_out.offset;
    }
    uint8_t* base = (uint8_t*) shmemGetAddr(&g_shared_memory);

    // Withdraw blocks left by an earlier run in this process.
    unsigned withdrawn = 0;
    for (size_t at = 0; at + sizeof(uint32_t) <= CMZ_SALTYNX_SHMEM_SIZE; at += 4) {
        uint32_t magic;
        memcpy(&magic, base + at, sizeof(magic));
        if (magic == CMZ_NXFPS_MAGIC && at != offset) {
            const uint32_t zero = 0;
            memcpy(base + at, &zero, sizeof(zero));
            ++withdrawn;
        }
    }

    CmzNxFpsBlock* block = (CmzNxFpsBlock*) (base + offset);
    memset(block, 0, sizeof(*block));
    block->API = CMZ_NXFPS_API_EGL;
    block->expectedSetBuffers = -1;
    // The magic goes last: readers search for it.
    atomic_thread_fence(memory_order_release);
    block->MAGIC = CMZ_NXFPS_MAGIC;
    atomic_store_explicit(&g_block, block, memory_order_release);

    if (reserved) {
        note("saltynx: publishing the frame rate at shared-memory offset %u%s", (unsigned) offset,
             withdrawn ? " (withdrew a stale block)" : "");
    } else {
        note("saltynx: no reservation (0x%X); publishing the frame rate at offset %u",
             reserve_rc, (unsigned) offset);
    }
}

void cmz_saltynx_start(void) {
    // Lowest application priority: it waits on SaltyNX and then exits.
    if (R_FAILED(threadCreate(&g_thread, connect_and_publish, NULL, NULL, 0x4000, 0x3B, -2))) {
        note("saltynx: thread not created");
        return;
    }
    if (R_FAILED(threadStart(&g_thread))) {
        threadClose(&g_thread);
        note("saltynx: thread not started");
    }
}

void cmz_saltynx_present(void) {
    CmzNxFpsBlock* block = atomic_load_explicit(&g_block, memory_order_acquire);
    if (block == NULL) {
        return;
    }
    const uint64_t now = armGetSystemTick();
    if (g_frame_end == 0) {
        g_frame_end = now;
        g_second_start = now;
    }
    const uint64_t delta = now - g_frame_end;
    g_frame_end = now;

    block->frameNumber = block->frameNumber + 1;
    block->FPSticks[g_tick_index] = delta > UINT32_MAX ? UINT32_MAX : (uint32_t) delta;
    g_tick_index = (g_tick_index + 1) % 10;
    g_frame_avg = g_frame_avg == 0 ? delta : (9 * g_frame_avg + delta) / 10;
    if (g_frame_avg > 0) {
        block->FPSavg = (float) CMZ_TICKS_PER_SECOND / (float) g_frame_avg;
    }

    // Frames per second, counted the way NX-FPS counts them.
    ++g_second_frames;
    const uint64_t elapsed = now - g_second_start;
    if (elapsed > CMZ_TICKS_PER_SECOND) {
        const uint32_t frames = g_second_frames - 1;
        block->FPS = (uint8_t) (frames > 255 ? 255 : frames);
        if (elapsed > 2 * CMZ_TICKS_PER_SECOND) {
            g_second_start = now;
            g_second_frames = 0;
        } else {
            g_second_start += CMZ_TICKS_PER_SECOND;
            g_second_frames = 1;
        }
    }
    block->pluginActive = true;
}

void cmz_saltynx_stop(void) {
    CmzNxFpsBlock* block = atomic_exchange_explicit(&g_block, NULL, memory_order_acq_rel);
    if (block != NULL) {
        block->MAGIC = 0;
        block->pluginActive = false;
    }
}
