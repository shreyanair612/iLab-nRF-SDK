#include <ctype.h>
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

#define MAX_ARGS 6

static const char *const help_lines[] = {
	"commands:",
	"  help | hello | status | config | reset",
	"  start | stop                       run with the current settings (BTN1 / BTN2 do the same)",
	"  set_duration <s>                   0 = until stop",
	"  set_angle <deg> | set_distance <cm> | set_label <text> | set_env <text> | set_clip <text>",
	"  set_delay <left> <center> <right>  beamformer delays in samples (62.5 us each)",
	"  set_gain <left> <center> <right>   per-mic Q15 weights, 32767 = 1.0",
	"  set_out_gain <q15>                 beamformer output gain",
	"  set_stream <left|center|right|bf> <on|off>",
	"  levels [on|off]                    print mic levels once, every second, or stop",
	"  binary_mode <on|off>               framed packets for the host tool",
};

static void ack(const char *fmt, ...)
{
	char msg[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	tt_send_text(TP_ACK, "ok: %s", msg);
}

static void fail(const char *fmt, ...)
{
	char msg[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	tt_send_text(TP_ERROR, "err: %s", msg);
}

static bool parse_int(const char *s, long min, long max, long *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || *end != '\0' || v < min || v > max) {
		return false;
	}
	*out = v;
	return true;
}

static bool parse_on_off(const char *s, bool *out)
{
	if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static bool require_idle(void)
{
	if (!run_controller_config_mutable()) {
		fail("stop the run first");
		return false;
	}
	return true;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);

	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
	/* Keep JSON payloads valid. */
	for (size_t i = 0; i < n; i++) {
		if (dst[i] == '"' || dst[i] == '\\') {
			dst[i] = '_';
		}
	}
}

static void handle(int argc, char *argv[], const char *rest_after_cmd)
{
	const char *cmd = argv[0];
	eval_run_config_t *cfg = run_controller_config();

	if (strcmp(cmd, "help") == 0) {
		for (size_t i = 0; i < ARRAY_SIZE(help_lines); i++) {
			tt_send_text(TP_TEXT, "%s", help_lines[i]);
		}
		ack("help");
	} else if (strcmp(cmd, "hello") == 0) {
		run_controller_emit_hello();
		ack("hello");
	} else if (strcmp(cmd, "status") == 0) {
		run_controller_emit_status();
		ack("status");
	} else if (strcmp(cmd, "config") == 0) {
		run_controller_emit_config();
		ack("config");
	} else if (strcmp(cmd, "reset") == 0) {
		if (!require_idle()) {
			return;
		}
		run_controller_reset();
		ack("reset");
	} else if (strcmp(cmd, "start") == 0) {
		char err[120];

		if (run_controller_start(err, sizeof(err)) == 0) {
			ack("start");
		} else {
			fail("%s", err);
		}
	} else if (strcmp(cmd, "stop") == 0) {
		if (run_controller_stop("command") == 0) {
			ack("stop");
		} else {
			fail("nothing to stop");
		}
	} else if (strcmp(cmd, "set_duration") == 0) {
		long v;

		if (argc != 2 || !parse_int(argv[1], 0, 3600, &v)) {
			fail("usage: set_duration <0..3600>");
			return;
		}
		if (!require_idle()) {
			return;
		}
		cfg->duration_s = (uint16_t)v;
		ack("duration %u s", cfg->duration_s);
	} else if (strcmp(cmd, "set_angle") == 0) {
		long v;

		if (argc != 2 || !parse_int(argv[1], -180, 180, &v)) {
			fail("usage: set_angle <-180..180>");
			return;
		}
		if (!require_idle()) {
			return;
		}
		cfg->angle_deg = (int16_t)v;
		ack("angle %d", cfg->angle_deg);
	} else if (strcmp(cmd, "set_distance") == 0) {
		long v;

		if (argc != 2 || !parse_int(argv[1], 0, 10000, &v)) {
			fail("usage: set_distance <cm>");
			return;
		}
		if (!require_idle()) {
			return;
		}
		cfg->distance_cm = (uint16_t)v;
		ack("distance %u cm", cfg->distance_cm);
	} else if (strcmp(cmd, "set_label") == 0 || strcmp(cmd, "set_env") == 0 ||
		   strcmp(cmd, "set_clip") == 0) {
		if (argc < 2) {
			fail("usage: %s <text>", cmd);
			return;
		}
		if (!require_idle()) {
			return;
		}
		if (cmd[4] == 'l') {
			copy_text(cfg->label, sizeof(cfg->label), rest_after_cmd);
			ack("label %s", cfg->label);
		} else if (cmd[4] == 'e') {
			copy_text(cfg->environment, sizeof(cfg->environment), rest_after_cmd);
			ack("env %s", cfg->environment);
		} else {
			copy_text(cfg->clip, sizeof(cfg->clip), rest_after_cmd);
			ack("clip %s", cfg->clip);
		}
	} else if (strcmp(cmd, "set_delay") == 0) {
		long d[EVAL_MIC_COUNT];

		if (argc != 4 || !parse_int(argv[1], 0, EVAL_MAX_DELAY_SAMPLES, &d[0]) ||
		    !parse_int(argv[2], 0, EVAL_MAX_DELAY_SAMPLES, &d[1]) ||
		    !parse_int(argv[3], 0, EVAL_MAX_DELAY_SAMPLES, &d[2])) {
			fail("usage: set_delay <left> <center> <right>  (0..%d samples)",
			     EVAL_MAX_DELAY_SAMPLES);
			return;
		}
		if (!require_idle()) {
			return;
		}
		for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
			cfg->bf.delay_samples[m] = (int16_t)d[m];
		}
		ack("delay left %ld center %ld right %ld", d[0], d[1], d[2]);
	} else if (strcmp(cmd, "set_gain") == 0) {
		long g[EVAL_MIC_COUNT];

		if (argc != 4 || !parse_int(argv[1], 0, 32767, &g[0]) ||
		    !parse_int(argv[2], 0, 32767, &g[1]) || !parse_int(argv[3], 0, 32767, &g[2])) {
			fail("usage: set_gain <left> <center> <right>  (0..32767)");
			return;
		}
		if (!require_idle()) {
			return;
		}
		for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
			cfg->bf.gain_q15[m] = (int16_t)g[m];
		}
		ack("gain left %ld center %ld right %ld", g[0], g[1], g[2]);
	} else if (strcmp(cmd, "set_out_gain") == 0) {
		long v;

		if (argc != 2 || !parse_int(argv[1], 0, 32767, &v)) {
			fail("usage: set_out_gain <0..32767>");
			return;
		}
		if (!require_idle()) {
			return;
		}
		cfg->bf.out_gain_q15 = (int16_t)v;
		ack("out_gain %ld", v);
	} else if (strcmp(cmd, "set_stream") == 0) {
		bool on;

		if (argc != 3 || !parse_on_off(argv[2], &on)) {
			fail("usage: set_stream <left|center|right|bf> <on|off>");
			return;
		}
		if (!require_idle()) {
			return;
		}
		if (strcmp(argv[1], "bf") == 0) {
			cfg->stream_bf = on;
		} else {
			int m = eval_mic_from_name(argv[1]);

			if (m < 0) {
				fail("unknown stream %s", argv[1]);
				return;
			}
			cfg->stream_raw[m] = on;
		}
		ack("stream %s %s", argv[1], on ? "on" : "off");
	} else if (strcmp(cmd, "levels") == 0) {
		if (argc == 1) {
			run_controller_request_levels(false, false);
			ack("levels once");
		} else if (strcmp(argv[1], "on") == 0) {
			run_controller_request_levels(true, false);
			ack("levels on");
		} else if (strcmp(argv[1], "off") == 0) {
			run_controller_request_levels(false, true);
			ack("levels off");
		} else {
			fail("usage: levels [on|off]");
		}
	} else if (strcmp(cmd, "binary_mode") == 0) {
		bool on;

		if (argc != 2 || !parse_on_off(argv[1], &on)) {
			fail("usage: binary_mode <on|off>");
			return;
		}
		/* Answer in the mode the host asked for, so the ack is parseable either way. */
		tt_set_binary_mode(on);
		ack("binary_mode %s", on ? "on" : "off");
	} else {
		fail("unknown command '%s' (try help)", cmd);
	}
}

void command_interface_handle_line(const char *line)
{
	char buf[256];
	char *argv[MAX_ARGS];
	int argc = 0;

	strncpy(buf, line, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	/* Trim. */
	char *s = buf;

	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	size_t n = strlen(s);

	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[--n] = '\0';
	}
	if (n == 0) {
		return;
	}

	/* Remember the text after the command word for free-text setters. */
	char rest[256];

	strcpy(rest, s);
	char *sp = strchr(rest, ' ');
	const char *rest_after_cmd = "";

	if (sp) {
		sp++;
		while (*sp == ' ') {
			sp++;
		}
		rest_after_cmd = sp;
	}

	char *tok = strtok(s, " \t");

	while (tok && argc < MAX_ARGS) {
		argv[argc++] = tok;
		tok = strtok(NULL, " \t");
	}
	if (argc == 0) {
		return;
	}
	handle(argc, argv, rest_after_cmd);
}

void command_interface_init(void)
{
	tt_set_rx_callbacks(command_interface_handle_line, NULL);
}
