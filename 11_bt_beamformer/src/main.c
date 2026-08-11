#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <dk_buttons_and_leds.h>

#include "beamform.h"
#include "bluetooth.h"

LOG_MODULE_REGISTER(main);

static atomic_t sending = ATOMIC_INIT(0);

static void try_send(void) {
	if(!beamform_has_audio() || !atomic_cas(&sending,0,1)) {
		return;
	}

	int err = bluetooth_send(beamform_audio(), beamform_audio_count());

	if (err) {
		LOG_ERR("Bluetooth send failed: %d", err);
	} else {
		beamform_clear();
		LOG_INF("Bluetooth transfer complete");
	}

	atomic_clear(&sending);
 }

 static void button_changed(uint32_t state, uint32_t changed) {
	uint32_t pressed = state & changed;

	if(pressed & DK_BTN1_MSK) {
		int err = beamform_start();
		if (err) {
			LOG_WRN("Recording not started: %d", err);
		}
	}
	if (pressed & DK_BTN2_MSK) {
		beamform_stop();
	}
 }

 int main(void) {
	int err = dk_leds_init();
	if(err) return err;
	err = dk_buttons_init(button_changed);
	if(err) return err;
	err = beamform_init(try_send);
	if(err) return err;
	err = bluetooth_init(try_send);
	if(err) return err;

	for (;;) {
		k_sleep(K_FOREVER);
	}
 }