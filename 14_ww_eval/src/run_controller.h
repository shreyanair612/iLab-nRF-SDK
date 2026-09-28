/*
 * Run controller: owns the run configuration and the evaluation thread that
 * turns queued microphone (or replay) frames into paths, metrics, wake-word
 * events, and telemetry.
 *
 * Every state change that touches the wake-word runtime or the metrics is
 * performed on the evaluation thread, ordered through the frame queue by
 * marker frames (start, stop, replay start, replay end). Commands only post
 * markers and edit the configuration while the controller is idle.
 */
#ifndef RUN_CONTROLLER_H_
#define RUN_CONTROLLER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eval_config.h"
#include "telemetry_protocol.h"

enum run_state {
	RUN_IDLE = 0,
	RUN_RUNNING,
	RUN_REPLAY,
};

int run_controller_init(void);

/* Configuration is only writable while idle. */
eval_run_config_t *run_controller_config(void);
bool run_controller_config_mutable(void);

enum run_state run_controller_state(void);
uint16_t run_controller_run_id(void);

/* Posts a start marker. Fails with -EBUSY if not idle, -EINVAL (message in
 * err) if the configuration cannot be carried by the link. */
int run_controller_start(char *err, size_t err_len);
int run_controller_stop(const char *reason);
void run_controller_reset(void);

int run_controller_replay_start(eval_path_id_t path, uint32_t total_frames, char *err,
				size_t err_len);
int run_controller_replay_end(void);

/* Called by the transport for TP_REPLAY_AUDIO / TP_REPLAY_END packets. */
void run_controller_on_packet(const struct tp_header *h, const uint8_t *payload);

/* 10 ms housekeeping from main: LEDs, idle status. */
void run_controller_tick(void);

void run_controller_emit_hello(void);
void run_controller_emit_status(void);
void run_controller_emit_config(void);
void run_controller_print_paths(void);

/* Text mode: print the raw microphones' level once, every second, or stop. */
void run_controller_request_levels(bool continuous, bool off);

/* Link budget for a configuration. Returns 0 if it fits, -EINVAL otherwise
 * (with a human message in msg). */
int run_controller_admission_check(const eval_run_config_t *cfg, char *msg, size_t msg_len,
				   uint32_t *bytes_per_s);

#endif /* RUN_CONTROLLER_H_ */
