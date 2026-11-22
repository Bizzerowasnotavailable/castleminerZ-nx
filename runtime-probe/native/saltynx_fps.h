#pragma once

// Frame-rate block for SaltyNX tools such as Status Monitor (see saltynx_fps.c).

#ifdef __cplusplus
extern "C" {
#endif

// Starts a background thread that, when SaltyNX is running, reserves an NX-FPS
// block in SaltyNX's shared memory. Returns immediately; never blocks the game.
void cmz_saltynx_start(void);

// Call once per presented frame on the presenting thread. Does nothing until
// the block exists, and is a single atomic load when SaltyNX is absent.
void cmz_saltynx_present(void);

// Withdraws the block at exit so it cannot be mistaken for a live game later.
void cmz_saltynx_stop(void);

#ifdef __cplusplus
}
#endif
