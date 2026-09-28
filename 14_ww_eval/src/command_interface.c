#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "command_interface.h"
#include "eval_config.h"
#include "run_controller.h"
#include "telemetry_transport.h"

LOG_MODULE_REGISTER(cmd, LOG_LEVEL_INF);

#define MAX_ARGS 8

static void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void ok(const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	tt_send_text(TP_ACK, "ok: %s", buf);
}

static void fail(const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	tt_send_text(TP_ACK, "err: %s", buf);
}

static const char *const help_lines[] = {
	"commands:",
	"  help | hello | status | config | list_paths | binary_mode on|off",
	"  start | stop | reset",
	"  enable <path> | disable <path> | enable_all | disable_all",
	"  set_preset left|right|lr_mix|bf_lr|raw_3ch|bf_3ch|raw_vs_bf_lr|raw_vs_bf_3ch|full",
	"  set_live_path <path>          path scored live during a run",
	"  set_duration <seconds>        0 = until stop",
	"  set_delay <path> <l> <r> <c>  integer samples, 0..max_delay",
	"  set_gain <path> <q15>         32767 = unity",
	"  set_threshold <0..1000|0.xx>  wake-word probability threshold",
	"  set_cooldown <ms>",
	"  set_capture <path|raw_left|raw_right|raw_center> on|off",
	"  set_capture_mode off|event_window|continuous_short|continuous_full",
	"  set_capture_seconds <s>",
	"  set_reduce center_only|lr_average|lc_average|rc_average|lrc_average",
	"  set_angle <deg> | set_distance <cm> | set_label <text> | set_phrase <text> | set_env <text>",
	"  replay_start <path> <frames> | replay_end",
	"  levels | levels on | levels off   raw microphone levels while idle (rig setup)",
	"paths: raw_left raw_right raw_lr_mix bf_lr raw_3ch bf_3ch",
};

static bool require_idle(void)
{
	if (!run_controller_config_mutable()) {
		fail("stop the run first");
		return false;
	}
	return true;
}

static int parse_path(const char *s)
{
	int p = eval_path_from_name(s);

	if (p < 0 && isdigit((unsigned char)s[0])) {
		p = atoi(s);
		if (p >= EVAL_PATH_COUNT) {
			p = -1;
		}
	}
	return p;
}

static bool parse_onoff(const char *s, bool *out)
{
	if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) {
		*out = true;
		return true;
	}
	if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) {
		*out = false;
		return true;
	}
	return false;
}

static void copy_rest(char *dst, size_t dst_len, int argc, char **argv, int from)
{
	dst[0] = '\0';
	for (int i = from; i < argc; i++) {
		size_t used = strlen(dst);

		if (used + strlen(argv[i]) + 2 > dst_len) {
			break;
		}
		if (used) {
			strcat(dst, "_");
		}
		strcat(dst, argv[i]);
	}
}

void command_interface_handle_line(const char *line_in)
{
	char line[256];
	char *argv[MAX_ARGS];
	int argc = 0;

	strncpy(line, line_in, sizeof(line) - 1);
	line[sizeof(line) - 1] = '\0';

	char *save = NULL;

	for (char *tok = strtok_r(line, " \t", &save); tok && argc < MAX_ARGS;
	     tok = strtok_r(NULL, " \t", &save)) {
		argv[argc++] = tok;
	}
	if (argc == 0) {
		return;
	}

	const char *cmd = argv[0];
	eval_run_config_t *cfg = run_controller_config();

	if (!strcmp(cmd, "help")) {
		for (size_t i = 0; i < ARRAY_SIZE(help_lines); i++) {
			tt_send_text(TP_TEXT, "%s", help_lines[i]);
		}
		ok("help");
	} else if (!strcmp(cmd, "hello")) {
		run_controller_emit_hello();
		ok("hello");
	} else if (!strcmp(cmd, "binary_mode")) {
		bool on;

		if (argc < 2 || !parse_onoff(argv[1], &on)) {
			fail("binary_mode on|off");
			return;
		}
		tt_set_binary_mode(on);
		ok("binary_mode %s", on ? "on" : "off");
		if (on) {
			run_controller_emit_hello();
			run_controller_emit_config();
		}
	} else if (!strcmp(cmd, "status")) {
		run_controller_emit_status();
		ok("status");
	} else if (!strcmp(cmd, "config")) {
		run_controller_emit_config();
		ok("config");
	} else if (!strcmp(cmd, "list_paths")) {
		run_controller_print_paths();
		ok("list_paths");
	} else if (!strcmp(cmd, "start")) {
		char err[160];
		int rc = run_controller_start(err, sizeof(err));

		if (rc) {
			fail("start: %s", err);
		} else {
			ok("start queued (%s)", err);
		}
	} else if (!strcmp(cmd, "stop")) {
		int rc = run_controller_stop("command");

		if (rc == -EALREADY) {
			fail("nothing running");
		} else if (rc) {
			fail("stop: %d", rc);
		} else {
			ok("stop queued");
		}
	} else if (!strcmp(cmd, "reset")) {
		if (!require_idle()) {
			return;
		}
		run_controller_reset();
		ok("reset to defaults (%s)", eval_preset_name(cfg->preset));
	} else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
		bool en = (cmd[0] == 'e');

		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("%s <path>", cmd);
			return;
		}
		if (!strcmp(argv[1], "all")) {
			for (int p = 0; p < EVAL_PATH_COUNT; p++) {
				cfg->path[p].enabled = en;
			}
			ok("%s all", cmd);
			return;
		}
		int p = parse_path(argv[1]);

		if (p < 0) {
			fail("unknown path %s", argv[1]);
			return;
		}
		cfg->path[p].enabled = en;
		ok("%s %s", cmd, eval_path_name((eval_path_id_t)p));
	} else if (!strcmp(cmd, "enable_all") || !strcmp(cmd, "disable_all")) {
		if (!require_idle()) {
			return;
		}
		for (int p = 0; p < EVAL_PATH_COUNT; p++) {
			cfg->path[p].enabled = (cmd[0] == 'e');
		}
		ok("%s", cmd);
	} else if (!strcmp(cmd, "set_preset")) {
		if (!require_idle()) {
			return;
		}
		int pr = (argc >= 2) ? eval_preset_from_name(argv[1]) : -1;

		if (pr < 0) {
			fail("unknown preset");
			return;
		}
		eval_run_config_apply_preset(cfg, (eval_preset_t)pr);
		ok("preset %s, live path %s", eval_preset_name(cfg->preset),
		   eval_path_name(cfg->live_path));
	} else if (!strcmp(cmd, "set_live_path")) {
		if (!require_idle()) {
			return;
		}
		int p = (argc >= 2) ? parse_path(argv[1]) : -1;

		if (p < 0) {
			fail("unknown path");
			return;
		}
		cfg->live_path = (eval_path_id_t)p;
		cfg->path[p].enabled = true;
		ok("live path %s", eval_path_name((eval_path_id_t)p));
	} else if (!strcmp(cmd, "set_duration")) {
		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("set_duration <seconds>");
			return;
		}
		cfg->duration_s = (uint16_t)atoi(argv[1]);
		ok("duration %u s", cfg->duration_s);
	} else if (!strcmp(cmd, "set_delay")) {
		if (!require_idle()) {
			return;
		}
		int p = (argc >= 4) ? parse_path(argv[1]) : -1;

		if (p < 0) {
			fail("set_delay <path> <left> <right> [center]");
			return;
		}
		int l = atoi(argv[2]), r = atoi(argv[3]), c = (argc >= 5) ? atoi(argv[4]) : 0;

		if (l < 0 || r < 0 || c < 0 || l > EVAL_MAX_DELAY_SAMPLES ||
		    r > EVAL_MAX_DELAY_SAMPLES || c > EVAL_MAX_DELAY_SAMPLES) {
			fail("delays must be 0..%d samples", (int)EVAL_MAX_DELAY_SAMPLES);
			return;
		}
		cfg->path[p].left_delay_samples = (int16_t)l;
		cfg->path[p].right_delay_samples = (int16_t)r;
		cfg->path[p].center_delay_samples = (int16_t)c;
		ok("%s delays l=%d r=%d c=%d", eval_path_name((eval_path_id_t)p), l, r, c);
	} else if (!strcmp(cmd, "set_gain")) {
		if (!require_idle()) {
			return;
		}
		int p = (argc >= 3) ? parse_path(argv[1]) : -1;
		int g = (argc >= 3) ? atoi(argv[2]) : -1;

		if (p < 0 || g < 0 || g > 32767) {
			fail("set_gain <path> <0..32767>");
			return;
		}
		cfg->path[p].gain_q15 = (int16_t)g;
		ok("%s gain %d", eval_path_name((eval_path_id_t)p), g);
	} else if (!strcmp(cmd, "set_threshold")) {
		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("set_threshold <0..1000 or 0.xx>");
			return;
		}
		int v;

		if (strchr(argv[1], '.')) {
			v = (int)(strtod(argv[1], NULL) * 1000.0 + 0.5);
		} else {
			v = atoi(argv[1]);
		}
		if (v < 0 || v > 1000) {
			fail("threshold out of range");
			return;
		}
		cfg->ww_threshold_x1000 = (uint16_t)v;
		ok("threshold %u/1000", cfg->ww_threshold_x1000);
	} else if (!strcmp(cmd, "set_cooldown")) {
		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("set_cooldown <ms>");
			return;
		}
		cfg->ww_cooldown_ms = (uint16_t)atoi(argv[1]);
		ok("cooldown %u ms", cfg->ww_cooldown_ms);
	} else if (!strcmp(cmd, "set_capture")) {
		if (!require_idle()) {
			return;
		}
		bool on;

		if (argc < 3 || !parse_onoff(argv[2], &on)) {
			fail("set_capture <path|raw_left|raw_right|raw_center> on|off");
			return;
		}
		/* raw_left / raw_right name the physical channels; the paths of the
		 * same name carry identical audio and are never streamed twice. */
		if (!strcmp(argv[1], "raw_center")) {
			cfg->capture_raw_center = on;
		} else if (!strcmp(argv[1], "raw_left")) {
			cfg->capture_raw_left = on;
		} else if (!strcmp(argv[1], "raw_right")) {
			cfg->capture_raw_right = on;
		} else {
			int p = parse_path(argv[1]);

			if (p < 0) {
				fail("unknown stream %s", argv[1]);
				return;
			}
			cfg->path[p].capture_audio = on;
		}
		char msg[160];
		uint32_t bps = 0;
		int rc = run_controller_admission_check(cfg, msg, sizeof(msg), &bps);

		ok("capture %s %s (%s%s)", argv[1], on ? "on" : "off", rc ? "WARNING " : "", msg);
	} else if (!strcmp(cmd, "set_capture_mode")) {
		if (!require_idle()) {
			return;
		}
		int m = (argc >= 2) ? eval_capture_mode_from_name(argv[1]) : -1;

		if (m < 0) {
			fail("set_capture_mode off|event_window|continuous_short|continuous_full");
			return;
		}
		cfg->capture_mode = (eval_capture_mode_t)m;
		ok("capture mode %s", eval_capture_mode_name(cfg->capture_mode));
	} else if (!strcmp(cmd, "set_capture_seconds")) {
		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("set_capture_seconds <s>");
			return;
		}
		cfg->capture_seconds = (uint16_t)atoi(argv[1]);
		ok("capture seconds %u", cfg->capture_seconds);
	} else if (!strcmp(cmd, "set_reduce")) {
		if (!require_idle()) {
			return;
		}
		int m = -1;

		for (int i = 0; argc >= 2 && i < RAW_3CH_REDUCE_COUNT; i++) {
			if (!strcmp(argv[1], eval_reduce_mode_name((raw_3ch_reduce_mode_t)i))) {
				m = i;
			}
		}
		if (m < 0) {
			fail("set_reduce center_only|lr_average|lc_average|rc_average|lrc_average");
			return;
		}
		cfg->raw_3ch_reduce = (raw_3ch_reduce_mode_t)m;
		ok("raw_3ch reduce %s", eval_reduce_mode_name(cfg->raw_3ch_reduce));
	} else if (!strcmp(cmd, "set_angle")) {
		if (!require_idle() || argc < 2) {
			if (argc < 2) {
				fail("set_angle <deg>");
			}
			return;
		}
		cfg->angle_deg = (int16_t)atoi(argv[1]);
		ok("angle %d", cfg->angle_deg);
	} else if (!strcmp(cmd, "set_distance")) {
		if (!require_idle() || argc < 2) {
			if (argc < 2) {
				fail("set_distance <cm>");
			}
			return;
		}
		cfg->distance_cm = (uint16_t)atoi(argv[1]);
		ok("distance %u cm", cfg->distance_cm);
	} else if (!strcmp(cmd, "set_label") || !strcmp(cmd, "set_phrase") || !strcmp(cmd, "set_env")) {
		if (!require_idle()) {
			return;
		}
		if (argc < 2) {
			fail("%s <text>", cmd);
			return;
		}
		char *dst = (cmd[4] == 'l') ? cfg->label : (cmd[4] == 'p') ? cfg->phrase : cfg->environment;
		size_t cap = (cmd[4] == 'l') ? sizeof(cfg->label) :
			     (cmd[4] == 'p') ? sizeof(cfg->phrase) : sizeof(cfg->environment);

		copy_rest(dst, cap, argc, argv, 1);
		ok("%s %s", cmd + 4, dst);
	} else if (!strcmp(cmd, "levels")) {
		bool on = argc >= 2 && !strcmp(argv[1], "on");
		bool off = argc >= 2 && !strcmp(argv[1], "off");

		if (!run_controller_config_mutable()) {
			fail("levels are printed during a run already");
			return;
		}
		run_controller_request_levels(on, off);
		ok("levels %s", off ? "off" : on ? "every second (levels off to stop)" : "once");
	} else if (!strcmp(cmd, "replay_start")) {
		int p = (argc >= 3) ? parse_path(argv[1]) : -1;
		long frames = (argc >= 3) ? atol(argv[2]) : -1;
		char err[120];

		if (p < 0 || frames <= 0) {
			fail("replay_start <path> <frames>");
			return;
		}
		int rc = run_controller_replay_start((eval_path_id_t)p, (uint32_t)frames, err, sizeof(err));

		if (rc) {
			fail("replay_start: %s", err);
		} else {
			ok("replay queued");
		}
	} else if (!strcmp(cmd, "replay_end")) {
		int rc = run_controller_replay_end();

		if (rc) {
			fail("no replay in progress");
		} else {
			ok("replay_end queued");
		}
	} else {
		fail("unknown command '%s' (try help)", cmd);
	}
}

static void on_line(const char *line)
{
	command_interface_handle_line(line);
}

static void on_packet(const struct tp_header *h, const uint8_t *payload)
{
	run_controller_on_packet(h, payload);
}

void command_interface_init(void)
{
	tt_set_rx_callbacks(on_line, on_packet);
}
