/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/logging/log.h>

#include "control_output.h"
#include "dmic.h"
#include "leds.h"
#include "wakeword.h"

LOG_MODULE_REGISTER(main);

int main(void)
{
	int err;

	err = dmic_init();
	if (err) {
		return err;
	}

	err = leds_init();
	if (err) {
		return err;
	}

	err = control_output_init();
	if (err) {
		return err;
	}

	err = ww_init();
	if (err) {
		return err;
	}

	LOG_INF("Initialization completed");

	err = dmic_start();
	if (err < 0) {
		LOG_ERR("Failed to start audio capture (err %d)", err);
		return err;
	}

	print_control_output((struct control_message){CONTROL_MESSAGE_WAITING_WW});

	while (true) {
		void *audio_buffer;
		size_t audio_buffer_size;
		bool ww_detected;

		const int32_t read_timeout = 100;

		err = dmic_read(&audio_buffer, &audio_buffer_size, read_timeout);
		if (err < 0) {
			LOG_ERR("Failed to read audio (err %d)", err);
			return err;
		}

		err = ww_process(audio_buffer, audio_buffer_size / DMIC_SAMPLE_BYTES, &ww_detected);
		if (err == -EBUSY) {
			continue;
		} else if (err < 0) {
			LOG_ERR("Wakeword detection failed (err %d)", err);
			return err;
		}

		if (ww_detected) {
			LOG_INF("Wake word detected");
			leds_blink_led0();
			print_control_output((struct control_message){CONTROL_MESSAGE_WW_DETECTED});
		}
	}

	return 0;
}