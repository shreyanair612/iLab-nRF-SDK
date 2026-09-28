/*
 * Host unit tests for the pure-C firmware modules. Build and run:
 *
 *   make -C tests_host
 *
 * These exercise exactly the objects that ship in the firmware, so a green
 * run here means the arithmetic on the device is what the host expects.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_metrics.h"
#include "beamformer_delay_sum.h"
#include "eval_config.h"
#include "eval_paths.h"
#include "telemetry_protocol.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                        \
	do {                                                                                    \
		checks++;                                                                       \
		if (!(cond)) {                                                                  \
			failures++;                                                             \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                             \
			printf(__VA_ARGS__);                                                    \
			printf("\n");                                                           \
		}                                                                               \
	} while (0)

/* ------------------------------------------------------------ mixing */

static void test_lr_mix_no_overflow(void)
{
	eval_run_config_t cfg;
	struct eval_paths ep;
	int16_t l[EVAL_FRAME_SAMPLES], r[EVAL_FRAME_SAMPLES];

	eval_run_config_defaults(&cfg);
	eval_run_config_apply_preset(&cfg, EVAL_PRESET_LR_MIX);
	eval_paths_init(&ep, &cfg);
	eval_paths_apply_config(&ep);
	eval_paths_reset_state(&ep);

	for (unsigned i = 0; i < EVAL_FRAME_SAMPLES; i++) {
		l[i] = INT16_MAX;
		r[i] = INT16_MAX;
	}
	struct eval_frame f = { .seq = 0, .sample_index = 0, .count = EVAL_FRAME_SAMPLES,
				.left = l, .right = r, .center = NULL };
	uint32_t mask = eval_paths_process(&ep, &f);

	CHECK(mask == (1U << EVAL_PATH_RAW_LR_MIX), "mask=%x", mask);
	/* 32767/2 + 32767/2 = 16383 + 16383 = 32766, no wraparound. */
	CHECK(ep.out[EVAL_PATH_RAW_LR_MIX][0] == 32766, "got %d", ep.out[EVAL_PATH_RAW_LR_MIX][0]);

	for (unsigned i = 0; i < EVAL_FRAME_SAMPLES; i++) {
		l[i] = INT16_MIN;
		r[i] = INT16_MIN;
	}
	eval_paths_process(&ep, &f);
	CHECK(ep.out[EVAL_PATH_RAW_LR_MIX][0] == INT16_MIN, "got %d", ep.out[EVAL_PATH_RAW_LR_MIX][0]);

	/* Truncation toward zero: -3/2 + 5/2 = -1 + 2 = 1 */
	l[0] = -3;
	r[0] = 5;
	eval_paths_process(&ep, &f);
	CHECK(ep.out[EVAL_PATH_RAW_LR_MIX][0] == 1, "got %d", ep.out[EVAL_PATH_RAW_LR_MIX][0]);
}

static void test_3ch_average(void)
{
	eval_run_config_t cfg;
	struct eval_paths ep;
	int16_t l[4] = { INT16_MAX, -3000, 0, 9 }, r[4] = { INT16_MAX, 3000, 0, 9 },
		c[4] = { INT16_MAX, 0, 0, 9 };

	eval_run_config_defaults(&cfg);
	eval_run_config_apply_preset(&cfg, EVAL_PRESET_RAW_3CH);
	cfg.raw_3ch_reduce = RAW_3CH_LRC_AVERAGE;
	eval_paths_init(&ep, &cfg);
	eval_paths_apply_config(&ep);
	eval_paths_reset_state(&ep);

	struct eval_frame f = { .count = 4, .left = l, .right = r, .center = c };
	uint32_t mask = eval_paths_process(&ep, &f);

	CHECK(mask & (1U << EVAL_PATH_RAW_3CH), "3ch not produced");
	CHECK(ep.out[EVAL_PATH_RAW_3CH][0] == 3 * (INT16_MAX / 3), "full scale avg %d",
	      ep.out[EVAL_PATH_RAW_3CH][0]);
	CHECK(ep.out[EVAL_PATH_RAW_3CH][1] == 0, "sym avg %d", ep.out[EVAL_PATH_RAW_3CH][1]);
	CHECK(ep.out[EVAL_PATH_RAW_3CH][3] == 9, "avg 9 -> %d", ep.out[EVAL_PATH_RAW_3CH][3]);

	cfg.raw_3ch_reduce = RAW_3CH_CENTER_ONLY;
	eval_paths_process(&ep, &f);
	CHECK(ep.out[EVAL_PATH_RAW_3CH][1] == 0 && ep.out[EVAL_PATH_RAW_3CH][3] == 9, "center only");

	/* Without a center channel the 3ch paths must report unavailable, not garbage. */
	f.center = NULL;
	mask = eval_paths_process(&ep, &f);
	CHECK((mask & (1U << EVAL_PATH_RAW_3CH)) == 0, "produced without center");
	CHECK(ep.unavailable_mask & (1U << EVAL_PATH_RAW_3CH), "not flagged unavailable");
}

/* -------------------------------------------------------- beamformer */

static void test_bf_zero_delay_equals_average(void)
{
	struct bf_delay_sum bf;
	int16_t l[8], r[8], out[8];
	const int16_t *in[2] = { l, r };

	bf_init(&bf, 2);
	bf_reset_history(&bf);
	for (int i = 0; i < 8; i++) {
		l[i] = (int16_t)(1000 * i);
		r[i] = (int16_t)(-500 * i);
	}
	bf_process(&bf, in, 8, out);
	for (int i = 0; i < 8; i++) {
		/* gain 32767/32768 rounds each term toward zero by at most 1 */
		int32_t expect = ((l[i] * 32767) >> 15) + ((r[i] * 32767) >> 15);

		expect /= 2;
		CHECK(out[i] == expect, "i=%d out=%d expect=%d", i, out[i], (int)expect);
	}
	CHECK(bf.clip_count == 0, "clips");
}

static void test_bf_saturation(void)
{
	struct bf_delay_sum bf;
	int16_t l[4] = { INT16_MAX, INT16_MIN, INT16_MAX, 0 };
	int16_t r[4] = { INT16_MAX, INT16_MIN, INT16_MIN, 0 };
	int16_t out[4];
	const int16_t *in[2] = { l, r };
	int16_t gains[2] = { INT16_MAX, INT16_MAX };

	bf_init(&bf, 2);
	bf_set_gains(&bf, gains, 2);
	bf_reset_history(&bf);
	bf_process(&bf, in, 4, out);
	/* Average of two full-scale samples stays in range; no clipping expected here. */
	CHECK(out[0] >= 32760 && out[0] <= INT16_MAX, "pos avg %d", out[0]);
	CHECK(out[1] <= -32760, "neg avg %d", out[1]);
	CHECK(out[2] == 0 || out[2] == -1, "cancel %d", out[2]);

	/* Full scale: the term (32767*32767)>>15 = 32766, so out hits 32766 twice /2 = 32766. */
	CHECK(bf.clip_count == 0, "unexpected clips %u", bf.clip_count);

	/* Force saturation with the 3-channel path and gain > average: not possible with
	 * equal-weight averaging, so verify the saturate helper directly. */
	CHECK(bf_saturate_int16(40000) == INT16_MAX, "sat hi");
	CHECK(bf_saturate_int16(-40000) == INT16_MIN, "sat lo");
}

static void test_bf_delay_sign_and_history(void)
{
	struct bf_delay_sum bf;
	int16_t l[6], r[6], out[6];
	const int16_t *in[2] = { l, r };
	int16_t delays[2] = { 2, 0 };
	int16_t gains[2] = { INT16_MAX, INT16_MAX };

	/* Right channel silent; left is an impulse at n=0. Delaying left by 2
	 * must move the impulse to n=2 in the output. */
	memset(r, 0, sizeof(r));
	memset(l, 0, sizeof(l));
	l[0] = 20000;

	bf_init(&bf, 2);
	CHECK(bf_set_delays(&bf, delays, 2) == 0, "set delays");
	bf_set_gains(&bf, gains, 2);
	bf_reset_history(&bf);
	bf_process(&bf, in, 6, out);
	CHECK(out[0] == 0 && out[1] == 0, "leading zeros %d %d", out[0], out[1]);
	CHECK(out[2] > 9000 && out[2] <= 10000, "impulse moved to n=2: %d", out[2]);

	/* History across frames: impulse at the END of frame 1 must show up at
	 * the START of frame 2 (delay 2 -> index 1 of the next frame). */
	memset(l, 0, sizeof(l));
	l[5] = 20000;
	bf_process(&bf, in, 6, out);
	CHECK(out[5] == 0, "no impulse in same frame at n=5: %d", out[5]);
	memset(l, 0, sizeof(l));
	bf_process(&bf, in, 6, out);
	CHECK(out[1] > 9000, "impulse carried into next frame: %d %d %d", out[0], out[1], out[2]);
	CHECK(out[0] == 0 && out[2] == 0, "only at n=1");

	/* Out-of-range delay is rejected. */
	int16_t bad[2] = { BF_MAX_DELAY + 1, 0 };

	CHECK(bf_set_delays(&bf, bad, 2) == -1, "bad delay accepted");
	int16_t neg[2] = { -1, 0 };

	CHECK(bf_set_delays(&bf, neg, 2) == -1, "negative delay accepted");
}

static void test_bf_reset_history_isolates_runs(void)
{
	struct bf_delay_sum bf;
	int16_t l[4] = { 0, 0, 0, 20000 }, r[4] = { 0 }, out[4];
	const int16_t *in[2] = { l, r };
	int16_t delays[2] = { 3, 0 };

	bf_init(&bf, 2);
	bf_set_delays(&bf, delays, 2);
	bf_reset_history(&bf);
	bf_process(&bf, in, 4, out);      /* impulse now in history */
	bf_reset_history(&bf);            /* new run: must not leak */
	memset(l, 0, sizeof(l));
	bf_process(&bf, in, 4, out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0, "history leaked");
}

/* ----------------------------------------------------------- metrics */

static void test_metrics(void)
{
	struct audio_metrics m;
	int16_t s[4] = { 100, -100, 100, -100 };

	am_reset(&m);
	am_update(&m, s, 4);
	CHECK(am_rms(&m.run) == 100, "rms %u", am_rms(&m.run));
	CHECK(m.run.peak_abs == 100, "peak");
	CHECK(m.run.clips == 0, "clips");

	int16_t c[2] = { INT16_MAX, INT16_MIN };

	am_update(&m, c, 2);
	CHECK(m.run.clips == 2, "clips %u", m.run.clips);
	CHECK(m.run.peak_abs == 32768, "peak %u", m.run.peak_abs);

	struct am_accum w;

	am_window_flush(&m, &w);
	CHECK(w.samples == 6, "window samples %u", w.samples);
	CHECK(m.window.samples == 0, "window cleared");

	/* CRC32 of "123456789" is the classic check value 0xCBF43926. */
	uint32_t crc = am_crc32_update(0, (const uint8_t *)"123456789", 9);

	CHECK(crc == 0xCBF43926U, "crc32 %08x", crc);
}

/* ---------------------------------------------------------- protocol */

static void test_protocol_roundtrip(void)
{
	uint8_t buf[TP_MAX_PACKET];
	struct tp_header h = { .type = TP_AUDIO_CHUNK, .flags = TP_FLAG_FINAL, .run_id = 7,
			       .stream_id = 1, .seq = 123456, .timestamp = 987654,
			       .payload_len = 0 };
	int16_t pcm[256];

	for (int i = 0; i < 256; i++) {
		pcm[i] = (int16_t)(i * 37 - 4000);
	}
	size_t n = tp_encode(buf, sizeof(buf), &h, pcm, sizeof(pcm));

	CHECK(n == TP_HEADER_LEN + sizeof(pcm) + TP_CRC_LEN, "encoded len %zu", n);
	CHECK(buf[0] == TP_MAGIC0 && buf[1] == TP_MAGIC1, "magic");

	struct tp_parser p;
	tp_parse_result_t res = TP_PARSE_NONE;

	tp_parser_init(&p);
	for (size_t i = 0; i < n; i++) {
		res = tp_parser_feed(&p, buf[i]);
		if (i < n - 1) {
			CHECK(res == TP_PARSE_NONE, "early result at %zu", i);
		}
	}
	CHECK(res == TP_PARSE_PACKET, "final result %d", res);
	CHECK(p.hdr.type == TP_AUDIO_CHUNK && p.hdr.run_id == 7 && p.hdr.stream_id == 1 &&
	      p.hdr.seq == 123456 && p.hdr.timestamp == 987654 && p.hdr.payload_len == 512,
	      "header fields");
	CHECK(memcmp(p.buf + TP_HEADER_LEN, pcm, sizeof(pcm)) == 0, "payload");

	/* Corrupt one byte: must report bad CRC and recover for the next packet. */
	buf[30] ^= 0x55;
	res = TP_PARSE_NONE;
	for (size_t i = 0; i < n; i++) {
		res = tp_parser_feed(&p, buf[i]);
	}
	CHECK(res == TP_PARSE_BAD_CRC, "bad crc not detected: %d", res);
	buf[30] ^= 0x55;
	for (size_t i = 0; i < n; i++) {
		res = tp_parser_feed(&p, buf[i]);
	}
	CHECK(res == TP_PARSE_PACKET, "did not recover after bad crc");

	/* Text lines are delivered as commands. */
	const char *line = "set_angle 45\n";

	for (const char *c = line; *c; c++) {
		res = tp_parser_feed(&p, (uint8_t)*c);
	}
	CHECK(res == TP_PARSE_TEXT_LINE, "text line %d", res);
	CHECK(strcmp((const char *)p.buf, "set_angle 45") == 0, "line content '%s'", p.buf);

	/* Oversize payload is refused by the encoder. */
	h.payload_len = 0;
	CHECK(tp_encode(buf, sizeof(buf), &h, pcm, TP_MAX_PAYLOAD + 1) == 0, "oversize accepted");

	/* Known CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
	CHECK(tp_crc16(0xFFFF, (const uint8_t *)"123456789", 9) == 0x29B1, "crc16 check value");
}

/* ------------------------------------------------------------- config */

static void test_presets_and_names(void)
{
	eval_run_config_t cfg;

	eval_run_config_defaults(&cfg);
	CHECK(cfg.preset == EVAL_PRESET_RAW_VS_BF_LR, "default preset");
	CHECK(eval_run_config_enabled_mask(&cfg) == 0x0F, "default mask %x",
	      eval_run_config_enabled_mask(&cfg));
	CHECK(cfg.live_path == EVAL_PATH_BF_LR, "live path %d", cfg.live_path);

	eval_run_config_apply_preset(&cfg, EVAL_PRESET_LEFT);
	CHECK(eval_run_config_enabled_mask(&cfg) == 0x01, "left mask");
	CHECK(cfg.live_path == EVAL_PATH_RAW_LEFT, "live path follows preset: %d", cfg.live_path);

	eval_run_config_apply_preset(&cfg, EVAL_PRESET_FULL);
	CHECK(eval_run_config_enabled_mask(&cfg) == 0x3F, "full mask");

	CHECK(eval_path_from_name("bf_lr") == EVAL_PATH_BF_LR, "name lookup");
	CHECK(eval_path_from_name("nope") == -1, "unknown name");
	CHECK(strcmp(eval_preset_name(EVAL_PRESET_RAW_VS_BF_3CH), "raw_vs_bf_3ch") == 0, "preset name");
	CHECK(eval_capture_mode_from_name("event_window") == EVAL_CAPTURE_EVENT_WINDOW, "mode name");
}

int main(void)
{
	test_lr_mix_no_overflow();
	test_3ch_average();
	test_bf_zero_delay_equals_average();
	test_bf_saturation();
	test_bf_delay_sign_and_history();
	test_bf_reset_history_isolates_runs();
	test_metrics();
	test_protocol_roundtrip();
	test_presets_and_names();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
