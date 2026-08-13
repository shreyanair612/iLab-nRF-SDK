#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <dk_buttons_and_leds.h>

#include "beamform.h"
#include "bluetooth.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

enum audio_state {
	STATE_IDLE = BIT(0),
	STATE_LISTENING = BIT(1),
	STATE_FINALIZE = BIT(2),
	STATE_PROCESSING = BIT(3),
	STATE_RESPONDING = BIT(4),
};

static atomic_t audio_state = ATOMIC_INIT(STATE_IDLE);
static atomic_t capture_enabled = ATOMIC_INIT(0);

static void enter_state(enum audio_state next_state) {
	atomic_set(&audio_state, next_state);

	switch(next_state) {
		case STATE_IDLE:
			atomic_clear(&capture_enabled);
			LOG_INF("STATE_IDLE");
			break;
		
		case STATE_LISTENING:
			atomic_set(&capture_enabled, 1);
			LOG_INF("STATE_LISTENING: capture_enabled=1");
			break;
		
		case STATE_FINALIZE:
			atomic_clear(&capture_enabled);
			LOG_INF("STATE_FINALIZE: capture_enabled=0, draining BLE TX queue");
			break;
		
		case STATE_PROCESSING:
			atomic_clear(&capture_enabled);
			LOG_INF("STATE_PROCESSING");
			break;
		
		case STATE_RESPONDING:
			atomic_clear(&capture_enabled);
			LOG_INF("STATE_RESPONDING");
			break;
		
		default:
			LOG_ERR("Invalid state: 0x%x", next_state);
			atomic_set(&audio_state, STATE_IDLE);
			atomic_clear(&capture_enabled);
			break;
	}
}

static void on_beamformed_chunk(const int16_t *samples, size_t count) {
	if (!atomic_get(&capture_enabled)) {
		return;
	}

	int err = bluetooth_enqueue_audio(samples, count);

	if(err) {
		LOG_WRN("BLE TX queue rejected audio chunk: %d", err);
	}
}

static void button_changed(uint32_t state, uint32_t changed) {
	uint32_t pressed = state & changed;
	enum audio_state current = atomic_get(&audio_state);

	// IDLE -> LISTENING
	if ((pressed & DK_BTN1_MSK) && current == STATE_IDLE) {
		enter_state(STATE_LISTENING);
	}
	
	// LISTENING -> FINALIZE
	if ((pressed & DK_BTN2_MSK) && current == STATE_LISTENING) {
		enter_state(STATE_FINALIZE);
	}
}

static void finalize_processing(void) {
	enum audio_state current = atomic_get(&audio_state);

	if(current == STATE_FINALIZE && bluetooth_tx_drained()) {
		enter_state(STATE_PROCESSING);
	}
}

int main(void) {
	int err = dk_leds_init();
	if(err) return err;
	err = dk_buttons_init(button_changed);
	if(err) return err;
	err = bluetooth_init();
	if(err) return err;
	err = beamform_init(on_beamformed_chunk);
	if(err) return err;
	err = beamform_start();
	if(err) {
		LOG_ERR("Always-on beamforming failed to start: %d", err);
		return err;
	}

	enter_state(STATE_IDLE);
	LOG_INF("Always-on beamforming active\nBTN1: IDLE->LISTENING, BTN2: LISTENING->FINALIZE");

	for (;;) {
		finalize_processing();
		k_sleep(K_MSEC(10));
	}
}