/*
 * 3-microphone beamforming evaluation firmware for the nRF54LM20 DK.
 *
 * Threads, highest priority first:
 *   audio_capture (6)  TDM DMA -> int16 L/C/R frames -> frame queue, never blocks
 *   eval          (7)  on-device delay-and-sum, level stats, audio streams to the host
 *   telemetry_rx  (8)  commands from the host
 *   main               10 ms housekeeping
 *
 * BTN1 starts a run with the current configuration, BTN2 stops it. Everything
 * else is driven over the USB link (see command_interface.c and README).
 * Zephyr logs go to uart20 (J-Link VCOM); audio and telemetry go to USB CDC ACM.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>

#include "audio_capture_adapter.h"
#include "command_interface.h"
#include "run_controller.h"
#include "telemetry_transport.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void button_changed(uint32_t state, uint32_t changed)
{
	uint32_t pressed = state & changed;

	if (pressed & DK_BTN1_MSK) {
		char err[120];

		if (run_controller_start(err, sizeof(err)) == 0) {
			LOG_INF("BTN1: start queued");
			tt_send_text(TP_TEXT, "BTN1: start");
		} else {
			LOG_WRN("BTN1: start refused: %s", err);
		}
	}
	if (pressed & DK_BTN2_MSK) {
		if (run_controller_stop("button") == 0) {
			LOG_INF("BTN2: stop queued");
		} else {
			LOG_WRN("BTN2: nothing to stop");
		}
	}
}

int main(void)
{
	int err;

	err = dk_leds_init();
	if (err) {
		return err;
	}
	err = dk_buttons_init(button_changed);
	if (err) {
		return err;
	}

	err = tt_init();
	if (err) {
		LOG_ERR("telemetry init failed: %d", err);
		for (;;) {
			dk_set_led_on(DK_LED1);
			dk_set_led_on(DK_LED2);
			k_sleep(K_MSEC(100));
			dk_set_led_off(DK_LED1);
			dk_set_led_off(DK_LED2);
			k_sleep(K_MSEC(100));
		}
	}
	command_interface_init();

	tt_send_text(TP_TEXT, "");
	tt_send_text(TP_TEXT, "=== 3-mic beamforming evaluation (16_3mic_eval) ===");
	tt_send_text(TP_TEXT, "type 'help' for commands, 'binary_mode on' for the host tool");

	err = audio_capture_init();
	if (err) {
		LOG_ERR("capture init failed: %d", err);
		tt_send_text(TP_ERROR, "err: capture init failed (%d)", err);
		return err;
	}

	err = run_controller_init();
	if (err) {
		return err;
	}

	err = audio_capture_start();
	if (err) {
		LOG_ERR("capture start failed: %d", err);
		return err;
	}

	LOG_INF("ready: %u Hz, %u mics, %u-sample frames; BTN1 start, BTN2 stop",
		audio_capture_sample_rate_hz(), audio_capture_channels(),
		(unsigned)EVAL_FRAME_SAMPLES);
	tt_send_text(TP_TEXT, "ready: %u Hz, %u mics, %u-sample frames; BTN1 start, BTN2 stop",
		     audio_capture_sample_rate_hz(), audio_capture_channels(),
		     (unsigned)EVAL_FRAME_SAMPLES);

	for (;;) {
		run_controller_tick();
		k_sleep(K_MSEC(10));
	}
}
