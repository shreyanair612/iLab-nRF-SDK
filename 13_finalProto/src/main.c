#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <dk_buttons_and_leds.h>

#include "beamform.h"
#include "bluetooth.h"
#include "vad.h"
#include "wakeword.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define MAX_STREAM_MS 30000U // never stopped speaking
#define NO_SPEECH_TIMEOUT_MS 10000U // never starts speaking
#define VAD_DEBUG_FRAMES_LISTENING 16U
#define VAD_DEBUG_FRAMES_IDLE 128U

enum audio_state {
	STATE_IDLE = BIT(0),
	STATE_LISTENING = BIT(1),
	STATE_FINALIZE = BIT(2),
	STATE_PROCESSING = BIT(3),
	STATE_RESPONDING = BIT(4),
};

enum stop_reason {
	STOP_NONE = 0,
	STOP_VAD_ENDPOINT,
	STOP_BUTTON,
	STOP_MAX_DURATION,
	STOP_NO_SPEECH,
};

enum start_reason {
	START_BUTTON = 0,
	START_WAKEWORD,
};

static const char *const start_reason_name[] = {
	[START_BUTTON] = "BTN1",
	[START_WAKEWORD] = "wake word",
};

static const char *const stop_reason_name[] = {
	[STOP_NONE] = "none",
	[STOP_VAD_ENDPOINT] = "VAD_ENDPOINT (automatic)",
	[STOP_BUTTON] = "BTN2 (manual failsafe)",
	[STOP_MAX_DURATION] = "max duration",
	[STOP_NO_SPEECH] = "no speech detected",
};

static atomic_t audio_state = ATOMIC_INIT(STATE_IDLE);
static atomic_t capture_enabled = ATOMIC_INIT(0);
static atomic_t last_stop_reason = ATOMIC_INIT(STOP_NONE);

/* Written by the capture thread, read by the main loop. */
static atomic_t speech_started = ATOMIC_INIT(0);
static atomic_t wakeword_ready = ATOMIC_INIT(0);
static atomic_t last_start_reason = ATOMIC_INIT(START_BUTTON);

static int64_t listening_start_ms;
static int64_t speech_start_ms;

/*
 * Preroll ring. Every frame captured while idle is kept here, and the whole
 * ring is flushed ahead of the live stream when a session opens. That covers
 * the wake-word model's confirmation delay, which would otherwise swallow the
 * first syllables of the command. Only the capture thread touches it: writes
 * happen while idle, and the flush is done by the capture thread on its first
 * recording frame rather than by whichever thread pressed the button.
 */
#define PREROLL_FRAMES ((CONFIG_APP_PREROLL_MS * 16000U) / (256U * 1000U))

#if PREROLL_FRAMES > 0
static int16_t preroll[PREROLL_FRAMES][256];
static uint32_t preroll_head;
static uint32_t preroll_used;
#endif
static atomic_t preroll_pending = ATOMIC_INIT(0);

static void preroll_store(const int16_t *samples, size_t count)
{
#if PREROLL_FRAMES > 0
	if (count != 256U) {
		return;
	}

	memcpy(preroll[preroll_head], samples, count * sizeof(int16_t));
	preroll_head = (preroll_head + 1U) % PREROLL_FRAMES;
	if (preroll_used < PREROLL_FRAMES) {
		preroll_used++;
	}
#else
	ARG_UNUSED(samples);
	ARG_UNUSED(count);
#endif
}

static void preroll_flush(void)
{
#if PREROLL_FRAMES > 0
	uint32_t start = (preroll_head + PREROLL_FRAMES - preroll_used) % PREROLL_FRAMES;
	uint32_t sent = 0U;

	for (uint32_t i = 0; i < preroll_used; i++) {
		uint32_t idx = (start + i) % PREROLL_FRAMES;

		if (bluetooth_enqueue_audio(preroll[idx], 256U) == 0) {
			sent++;
		}
	}

	LOG_INF("Preroll: sent %u of %u frames (%u ms)", sent, preroll_used,
		sent * 256U * 1000U / 16000U);
	preroll_used = 0U;
#endif
}

static const struct vad_config vad_config = {
	.sample_rate_hz = CONFIG_APP_VAD_SAMPLE_RATE_HZ,
	.noise_init_rms = CONFIG_APP_VAD_NOISE_INIT_RMS,
	.onset_ratio_x8 = CONFIG_APP_VAD_ONSET_RATIO_X8,
	.off_ratio_x8 = CONFIG_APP_VAD_OFF_RATIO_X8,
	.margin_on = CONFIG_APP_VAD_MARGIN_ON,
	.margin_off = CONFIG_APP_VAD_MARGIN_OFF,
	.onset_ms = CONFIG_APP_VAD_ONSET_MS,
	.silence_end_ms = CONFIG_APP_VAD_SILENCE_END_MS,
	.min_speech_ms = CONFIG_APP_VAD_MIN_SPEECH_MS,
	.noise_alpha_shift = CONFIG_APP_VAD_NOISE_ALPHA_SHIFT,
};

static void log_vad_telemetry(const char *tag, bool listening)
{
	struct vad_debug d;

	vad_get_debug(&d);

	LOG_INF("VAD[%s] frame=%u smp/%u B (%u ms @%u Hz, 1 ch) rms=%u noise=%u "
		"on=%u off=%u -> %s | speech_active=%d onset=%u/%u silence=%u/%u "
		"speech=%u/%u latched=%d listening=%d",
		tag, d.frame_samples, d.frame_bytes, d.frame_ms, d.sample_rate_hz,
		d.rms, d.noise_rms, d.threshold_on, d.threshold_off,
		d.frame_is_speech ? "SPEECH" : "silence",
		d.speech_active ? 1 : 0, d.onset_frames, d.onset_frames_req,
		d.silence_frames, d.silence_end_frames,
		d.speech_frames, d.min_speech_frames, d.endpoint_latched ? 1 : 0,
		listening ? 1 : 0);
}

static void enter_state(enum audio_state next_state);

static bool request_finalize(enum stop_reason reason)
{
	if (!atomic_cas(&audio_state, STATE_LISTENING, STATE_FINALIZE)) {
		return false;
	}

	atomic_clear(&capture_enabled);
	atomic_set(&last_stop_reason, reason);

	LOG_INF("Recording stop: %s (after %lld ms)", stop_reason_name[reason],
		k_uptime_get() - listening_start_ms);
	LOG_INF("STATE_FINALIZE: capture disabled, draining BLE TX queue");

	return true;
}

static bool request_listen(enum start_reason reason)
{
	if (!atomic_cas(&audio_state, STATE_IDLE, STATE_LISTENING)) {
		return false;
	}

	atomic_set(&last_start_reason, reason);
	enter_state(STATE_LISTENING);

	return true;
}

static void enter_state(enum audio_state next_state)
{
	atomic_set(&audio_state, next_state);

	switch (next_state) {
	case STATE_IDLE:
		atomic_clear(&capture_enabled);
		ww_reset();
		LOG_INF("STATE_IDLE: say the wake word or press BTN1");
		break;

	case STATE_LISTENING:
		vad_reset_endpoint();
		ww_reset();
		bluetooth_tx_reset_stats();
		atomic_set(&preroll_pending, 1);
		atomic_clear(&speech_started);
		atomic_set(&last_stop_reason, STOP_NONE);
		listening_start_ms = k_uptime_get();
		speech_start_ms = 0;
		atomic_set(&capture_enabled, 1);
		LOG_INF("STATE_LISTENING: started by %s, waiting for speech, "
			"streaming enabled",
			start_reason_name[atomic_get(&last_start_reason)]);
		break;

	case STATE_FINALIZE:
		atomic_clear(&capture_enabled);
		LOG_INF("STATE_FINALIZE: capture disabled, draining BLE TX queue");
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

static void on_beamformed_chunk(const int16_t *samples, size_t count)
{
	static uint32_t frames_since_log;
	enum audio_state current;
	enum vad_event event;
	bool listening;
	int err;

	current = (enum audio_state)atomic_get(&audio_state);
	listening = (current == STATE_LISTENING) && atomic_get(&capture_enabled);

	event = vad_process(samples, count, listening);

	if (!listening && current == STATE_IDLE && atomic_get(&wakeword_ready)) {
		bool ww_detected = false;

		err = ww_process(samples, (uint16_t)count, &ww_detected);
		if (err && err != -EBUSY) {
			LOG_WRN("Wakeword inference failed: %d", err);
		} else if (ww_detected) {
			LOG_INF("Wake word detected");
			request_listen(START_WAKEWORD);
			return;
		}
	}

	if (IS_ENABLED(CONFIG_APP_VAD_DEBUG)) {
		uint32_t period = listening ? VAD_DEBUG_FRAMES_LISTENING
					    : VAD_DEBUG_FRAMES_IDLE;

		if (++frames_since_log >= period) {
			frames_since_log = 0U;
			log_vad_telemetry(listening ? "rec" : "idle", listening);
		}
	}

	if (!listening) {
		preroll_store(samples, count);
		return;
	}

	if (atomic_cas(&preroll_pending, 1, 0)) {
		preroll_flush();
	}

	switch (event) {
	case VAD_SPEECH_STARTED:
		atomic_set(&speech_started, 1);
		speech_start_ms = k_uptime_get();
		LOG_INF("Speech onset detected (%lld ms after start)",
			speech_start_ms - listening_start_ms);
		if (IS_ENABLED(CONFIG_APP_VAD_DEBUG)) {
			log_vad_telemetry("onset", true);
		}
		break;

	case VAD_ENDPOINT:
		LOG_INF("VAD endpoint: %u ms of continuous silence after speech",
			CONFIG_APP_VAD_SILENCE_END_MS);
		if (IS_ENABLED(CONFIG_APP_VAD_DEBUG)) {
			log_vad_telemetry("endpoint", true);
		}
		
		(void)bluetooth_enqueue_audio(samples, count);
		request_finalize(STOP_VAD_ENDPOINT);
		return;

	case VAD_NO_EVENT:
	default:
		break;
	}

	err = bluetooth_enqueue_audio(samples, count);
	if (err && err != -ENOTCONN) {
		LOG_WRN("BLE TX queue rejected audio chunk: %d", err);
	}
}


// Button handler. BTN1 starts a session, BTN2 is the manual failsafe stop.
static void button_changed(uint32_t state, uint32_t changed)
{
	uint32_t pressed = state & changed;
	enum audio_state current = (enum audio_state)atomic_get(&audio_state);

	if (pressed & DK_BTN1_MSK) {
		if (request_listen(START_BUTTON)) {
			LOG_INF("BTN1 start requested");
		} else {
			LOG_WRN("BTN1 ignored: session already active (state 0x%x)",
				current);
		}
		return;
	}

	if (pressed & DK_BTN2_MSK) {
		if (!request_finalize(STOP_BUTTON)) {
			LOG_WRN("BTN2 ignored: not recording (state 0x%x)", current);
		}
	}
}

static void finalize_processing(void)
{
	enum audio_state current = (enum audio_state)atomic_get(&audio_state);
	uint32_t dropped;

	if (current == STATE_LISTENING) {
		int64_t elapsed = k_uptime_get() - listening_start_ms;

		if (elapsed >= MAX_STREAM_MS) {
			LOG_WRN("Maximum stream duration reached");
			request_finalize(STOP_MAX_DURATION);
		} else if (!atomic_get(&speech_started) &&
			   elapsed >= NO_SPEECH_TIMEOUT_MS) {
			LOG_WRN("No speech within %u ms", NO_SPEECH_TIMEOUT_MS);
			request_finalize(STOP_NO_SPEECH);
		}
		return;
	}

	if (current != STATE_FINALIZE || !bluetooth_tx_drained()) {
		return;
	}

	dropped = bluetooth_tx_dropped_chunks();
	if (dropped) {
		LOG_WRN("Finalization: %u audio chunks were dropped; "
			"the received recording has gaps", dropped);
	}

	LOG_INF("Finalization complete: reason=%s, %lld ms captured, "
		"speech_detected=%d",
		stop_reason_name[atomic_get(&last_stop_reason)],
		k_uptime_get() - listening_start_ms,
		atomic_get(&speech_started) ? 1 : 0);

	// post recording
	enter_state(STATE_PROCESSING);
	enter_state(STATE_IDLE);
}

int main(void)
{
	int err = dk_leds_init();

	if (err) {
		return err;
	}

	err = dk_buttons_init(button_changed);
	if (err) {
		return err;
	}

	err = vad_init(&vad_config);
	if (err) {
		LOG_ERR("VAD initialization failed: %d", err);
		return err;
	}

	err = ww_init();
	if (err) {
		LOG_ERR("Wakeword initialization failed: %d", err);
		return err;
	}
	atomic_set(&wakeword_ready, 1);

	err = bluetooth_init();
	if (err) {
		LOG_ERR("Bluetooth initialization failed: %d", err);
		return err;
	}

	err = beamform_init(on_beamformed_chunk);
	if (err) {
		LOG_ERR("Beamformer initialization failed: %d", err);
		return err;
	}

	err = beamform_start();
	if (err) {
		LOG_ERR("Always-on beamforming failed to start: %d", err);
		return err;
	}

	LOG_INF("Beamforming + wake word + adaptive VAD active");
	LOG_INF("VAD: %u Hz, endpoint after %u ms silence, min utterance %u ms",
		(uint32_t)CONFIG_APP_VAD_SAMPLE_RATE_HZ,
		(uint32_t)CONFIG_APP_VAD_SILENCE_END_MS,
		(uint32_t)CONFIG_APP_VAD_MIN_SPEECH_MS);
	LOG_INF("VAD thresholds: on=%u/8x off=%u/8x of noise floor "
		"(min +%u / +%u), noise init=%u, preroll %u frames",
		(uint32_t)CONFIG_APP_VAD_ONSET_RATIO_X8,
		(uint32_t)CONFIG_APP_VAD_OFF_RATIO_X8,
		(uint32_t)CONFIG_APP_VAD_MARGIN_ON,
		(uint32_t)CONFIG_APP_VAD_MARGIN_OFF,
		(uint32_t)CONFIG_APP_VAD_NOISE_INIT_RMS,
		(uint32_t)PREROLL_FRAMES);
	LOG_INF("Wakeword: prob>%u/1000, %u of last %u windows",
		(uint32_t)CONFIG_WW_PROBABILITY_THRESHOLD,
		(uint32_t)CONFIG_WW_COUNT_THRESHOLD,
		(uint32_t)CONFIG_WW_HISTORY_SIZE);
	LOG_INF("Wake word or BTN1: start recording   BTN2: manual failsafe stop");

	enter_state(STATE_IDLE);

	for (;;) {
		finalize_processing();
		k_sleep(K_MSEC(10));
	}
}
