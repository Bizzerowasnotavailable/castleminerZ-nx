#pragma once

// castleminerZ-nx performance monitor.
//
// Writes sdmc:/switch/castleminerz-logs/perf.log every few seconds: frame
// pacing, time spent in FNA3D by category, Mono GC pauses, and CPU time of the
// main thread and of every managed thread. When
// sdmc:/switch/castleminerz-logs/profile.ini exists it also samples call stacks
// into profile.bin. See docs/performance.md for the fields and the analysis
// script.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CmzPerfLogFn)(const char* line);

// Call once on the main thread before Mono starts. Never fails: problems are
// written to the host log and the affected statistics are left out.
void cmz_perf_initialize(CmzPerfLogFn host_log);

// Installs the Mono profiler callbacks (GC pauses, managed thread start, name
// and stop). Call after mono_jit_init and before mono_jit_exec.
void cmz_perf_attach_runtime(void);

#ifdef __cplusplus
}
#endif
