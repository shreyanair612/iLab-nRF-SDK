#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <dk_buttons_and_leds.h>

#include "beamform.h"
#include "bluetooth.h"
#include "vad.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define MAX_STREAM_MS 30000U
#define VAD_DEBUG_BLOCKS 32U

enum audio_state {
	STATE_IDLE = BIT(0),
	STATE_LISTENING = BIT(1),
	STATE_FINALIZE = BIT(2),
	STATE_PROCESSING = BIT(3),
	STATE_RESPONDING = BIT(4),
};

static atomic_t audio_state = ATOMIC_INIT(STATE_IDLE);
static atomic_t capture_enabled = ATOMIC_INIT(0);
static int64_t listening_start_ms;

static const struct vad_config vad_config = {
	.sample_rate_hz = 16000U,
	.noise_init_rms = 25U,
	.margin_off = 70U,
	.silence_end_ms = 1500U,
	.noise_alpha_shift = 5U,
};

static void enter_state(enum audio_state next_state) {
	atomic_set(&audio_state, next_state);

	switch(next_state) {
	case STATE_IDLE:
		atomic_clear(&capture_enabled);
		LOG_INF("STATE_IDLE");
		break;
		
	case STATE_LISTENING:
		vad_reset_endpoint();
		listening_start_ms = k_uptime_get();
		atomic_set(&capture_enabled, 1);
		LOG_INF("STATE_LISTENING: streaming enabled");
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
	static uint32_t debug_blocks;
	enum audio_state current;
	enum vad_event event;
	struct vad_debug debug;
	bool listening;
	int err;

	current = atomic_get(&audio_state);
	listening = (current == STATE_LISTENING) && atomic_get(&capture_enabled);
	event = vad_process(samples, count, listening);

	if(++debug_blocks>= VAD_DEBUG_BLOCKS) {
		debug_blocks=0U;
		vad_get_debug(&debug);

		LOG_INF("VAD: rms=%u noise=%u off=%u silence=%u/%u listening=%d", 
			debug.rms, debug.noise_rms, debug.threshold_off,
			debug.silence_frames, debug.silence_end_frames,
			listening ? 1 : 0);
	}

	if (!listening) return;

	if(event == VAD_ENDPOINT) {
		LOG_INF("VAD endpoint: ignored during BLE transport test");
		// LOG_INF("VAD endpoint: finalizing stream");
		// enter_state(STATE_FINALIZE);
		// return;
	}

	err = bluetooth_enqueue_audio(samples, count);
	if(err && err != -ENOTCONN) {
		LOG_WRN("BLE TX queue rejected audio chunk: %d", err);
	}
}

static void button_changed(uint32_t state, uint32_t changed) {
	uint32_t pressed = state & changed;
	enum audio_state current = atomic_get(&audio_state);

	// IDLE -> LISTENING
	if ((pressed & DK_BTN1_MSK) && current == STATE_IDLE) {
		enter_state(STATE_LISTENING);
		return;
	}
	
	// LISTENING -> FINALIZE
	if ((pressed & DK_BTN2_MSK) && current == STATE_LISTENING) {
		LOG_WRN("BTN2 manual stop: finalizing stream");
		enter_state(STATE_FINALIZE);
	}
}

static void finalize_processing(void) {
	enum audio_state current = atomic_get(&audio_state);

	if(current == STATE_LISTENING && (k_uptime_get() - listening_start_ms) >= MAX_STREAM_MS) {
		LOG_WRN("Maximum stream duration reached: finalizing stream");
		enter_state(STATE_FINALIZE);
		return;
	}

	if(current != STATE_FINALIZE || !bluetooth_tx_drained()) {
		return;
	}
	
	enter_state(STATE_PROCESSING);

	enter_state(STATE_IDLE);
}

int main(void) {
	int err = dk_leds_init();
	if(err) return err;
	err = dk_buttons_init(button_changed);
	if(err) return err;

	err = vad_init(&vad_config);
	if (err) { 
		LOG_ERR("VAD initialization failed: %d", err);
		return err;
	}

	err = bluetooth_init();
	if(err) {
		LOG_ERR("Bluetooth initialization failed: %d", err);
		return err;
	}

	err = beamform_init(on_beamformed_chunk);
	if (err) {
		LOG_ERR("Beamformer initilization failed: %d", err);
		return err;
	}
	
	err = beamform_start();
	if(err) {
		LOG_ERR("Always-on beamforming failed to start: %d", err);
		return err;
	}

	enter_state(STATE_IDLE);
	LOG_INF("Beamforming & adaptive VAD active");
	LOG_INF("BTN1: begin listening");

	for (;;) {
		finalize_processing();
		k_sleep(K_MSEC(10));
	}
}