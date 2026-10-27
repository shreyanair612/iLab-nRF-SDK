/*
 * Run controller: owns the run configuration and the evaluation thread that
 * turns queued microphone frames into the on-device beamformer output,
 * per-stream level statistics and the audio streams sent to the host.
 *
 * A "run" is one recording: it starts on BTN1 or `start`, streams the raw
 * microphones (and the beamformer output) continuously, and ends after the
 * configured duration, on BTN2 or on `stop`. Everything that touches the run
 * state happens on the evaluation thread, ordered with the audio frames.
 */
#ifndef RUN_CONTROLLER_H_
#define RUN_CONTROLLER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eval_config.h"

enum run_state {
	RUN_IDLE = 0,
	RUN_RUNNING,
};

int run_controller_init(void);

/* Configuration is only writable while idle. */
eval_run_config_t *run_controller_config(void);
bool run_controller_config_mutable(void);

enum run_state run_controller_state(void);
uint16_t run_controller_run_id(void);

/* Requests a start. Fails with -EBUSY if a run is active. */
int run_controller_start(char *err, size_t err_len);
int run_controller_stop(const char *reason);
void run_controller_reset(void);

/* 10 ms housekeeping from main: LEDs, idle status. */
void run_controller_tick(void);

void run_controller_emit_hello(void);
void run_controller_emit_status(void);
void run_controller_emit_config(void);

/* Text mode: print the raw microphones' level once, every second, or stop. */
void run_controller_request_levels(bool continuous, bool off);

#endif /* RUN_CONTROLLER_H_ */
