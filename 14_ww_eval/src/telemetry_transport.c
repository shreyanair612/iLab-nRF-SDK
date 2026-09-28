#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "eval_config.h"
#include "telemetry_transport.h"

#ifndef CONFIG_EVAL_TX_RING_BYTES
#define CONFIG_EVAL_TX_RING_BYTES 32768
#endif
#ifndef CONFIG_EVAL_TEXT_MODE_AT_BOOT
#define CONFIG_EVAL_TEXT_MODE_AT_BOOT 1
#endif

#define UART_NODE       DT_NODELABEL(uart20)
#define TX_CHUNK_BYTES  1024
#define RX_BUF_BYTES    256
#define RX_RING_BYTES   4096
#define RX_IDLE_US      1000

static const struct device *const uart = DEVICE_DT_GET(UART_NODE);

/* ------------------------------------------------------------ TX side */

RING_BUF_DECLARE(tx_ring, CONFIG_EVAL_TX_RING_BYTES);
static struct k_spinlock tx_lock;
static K_SEM_DEFINE(tx_data_sem, 0, 1);
static K_SEM_DEFINE(tx_done_sem, 0, 1);
static uint8_t tx_dma[TX_CHUNK_BYTES] __aligned(4);
static atomic_t binary_mode = ATOMIC_INIT(CONFIG_EVAL_TEXT_MODE_AT_BOOT ? 0 : 1);
static struct tt_stats stats = { .ring_size = CONFIG_EVAL_TX_RING_BYTES };
static volatile bool in_panic;

K_THREAD_STACK_DEFINE(tx_stack, 1024);
static struct k_thread tx_thread_data;

/* All-or-nothing enqueue. Safe from any thread and from the log thread. */
static int ring_put_atomic(const uint8_t *data, size_t len, bool is_audio)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);
	int rc;

	if (ring_buf_space_get(&tx_ring) < len) {
		stats.packets_dropped++;
		if (is_audio) {
			stats.audio_dropped++;
		}
		rc = -ENOSPC;
	} else {
		ring_buf_put(&tx_ring, data, (uint32_t)len);
		stats.packets_sent++;
		stats.bytes_sent += (uint32_t)len;
		uint32_t used = ring_buf_size_get(&tx_ring);

		if (used > stats.ring_high_water) {
			stats.ring_high_water = used;
		}
		rc = 0;
	}
	k_spin_unlock(&tx_lock, key);
	if (rc == 0) {
		k_sem_give(&tx_data_sem);
	}
	return rc;
}

static void tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&tx_data_sem, K_FOREVER);

		for (;;) {
			uint32_t n;
			k_spinlock_key_t key = k_spin_lock(&tx_lock);

			n = ring_buf_get(&tx_ring, tx_dma, sizeof(tx_dma));
			k_spin_unlock(&tx_lock, key);
			if (n == 0) {
				break;
			}
			if (uart_tx(uart, tx_dma, n, SYS_FOREVER_US) == 0) {
				k_sem_take(&tx_done_sem, K_FOREVER);
			} else {
				/* Driver busy or failed; poll out so nothing is lost silently. */
				for (uint32_t i = 0; i < n; i++) {
					uart_poll_out(uart, tx_dma[i]);
				}
			}
		}
	}
}

/* ------------------------------------------------------------ RX side */

RING_BUF_DECLARE(rx_ring, RX_RING_BYTES);
static uint8_t rx_bufs[2][RX_BUF_BYTES] __aligned(4);
static uint8_t rx_next;
static K_SEM_DEFINE(rx_sem, 0, 1);
static struct k_work rx_restart_work;
static tt_rx_line_cb_t line_cb;
static tt_rx_packet_cb_t packet_cb;
static struct tp_parser parser;

K_THREAD_STACK_DEFINE(rx_stack, 2048);
static struct k_thread rx_thread_data;

static void rx_restart(struct k_work *w)
{
	ARG_UNUSED(w);
	rx_next = 0;
	(void)uart_rx_enable(uart, rx_bufs[0], RX_BUF_BYTES, RX_IDLE_US);
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done_sem);
		break;
	case UART_RX_RDY: {
		const uint8_t *p = evt->data.rx.buf + evt->data.rx.offset;
		uint32_t n = ring_buf_put(&rx_ring, p, evt->data.rx.len);

		if (n < evt->data.rx.len) {
			stats.rx_overflow += evt->data.rx.len - n;
		}
		stats.rx_bytes += n;
		k_sem_give(&rx_sem);
		break;
	}
	case UART_RX_BUF_REQUEST:
		rx_next ^= 1;
		(void)uart_rx_buf_rsp(uart, rx_bufs[rx_next], RX_BUF_BYTES);
		break;
	case UART_RX_DISABLED:
		k_work_submit(&rx_restart_work);
		break;
	default:
		break;
	}
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint8_t byte;

		k_sem_take(&rx_sem, K_FOREVER);
		while (ring_buf_get(&rx_ring, &byte, 1) == 1) {
			tp_parse_result_t r = tp_parser_feed(&parser, byte);

			if (r == TP_PARSE_TEXT_LINE) {
				if (line_cb) {
					line_cb((const char *)parser.buf);
				}
			} else if (r == TP_PARSE_PACKET) {
				stats.rx_packets++;
				if (parser.hdr.type == TP_COMMAND) {
					/* Command text arrives without a terminator; NUL it. */
					uint16_t n = parser.hdr.payload_len;

					if (n < TP_MAX_PAYLOAD) {
						parser.buf[TP_HEADER_LEN + n] = '\0';
					}
					if (line_cb) {
						line_cb((const char *)(parser.buf + TP_HEADER_LEN));
					}
				} else if (packet_cb) {
					packet_cb(&parser.hdr, parser.buf + TP_HEADER_LEN);
				}
			} else if (r == TP_PARSE_BAD_CRC) {
				stats.rx_bad_crc++;
			}
		}
		stats.rx_resyncs = parser.resyncs;
	}
}

/* ------------------------------------------------------- log backend */

static char log_line[256];
static size_t log_line_len;
static uint8_t log_out_buf[64];

static int log_char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	if (in_panic) {
		for (size_t i = 0; i < length; i++) {
			uart_poll_out(uart, data[i]);
		}
		return (int)length;
	}

	for (size_t i = 0; i < length; i++) {
		char c = (char)data[i];

		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			log_line[log_line_len] = '\0';
			if (tt_send_text(TP_LOG, "%s", log_line) != 0) {
				stats.log_lines_dropped++;
			}
			log_line_len = 0;
			continue;
		}
		if (log_line_len < sizeof(log_line) - 1) {
			log_line[log_line_len++] = c;
		}
	}
	return (int)length;
}

LOG_OUTPUT_DEFINE(tt_log_output, log_char_out, log_out_buf, sizeof(log_out_buf));

static void log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	uint32_t flags = log_backend_std_get_flags();

	log_output_msg_process(&tt_log_output, &msg->log, flags);
}

static void log_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	in_panic = true;
	log_backend_std_panic(&tt_log_output);
}

static void log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	log_backend_std_dropped(&tt_log_output, cnt);
}

static void log_init_backend(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	log_output_ctx_set(&tt_log_output, NULL);
}

static const struct log_backend_api tt_log_api = {
	.process = log_process,
	.panic = log_panic,
	.dropped = log_dropped,
	.init = log_init_backend,
};

LOG_BACKEND_DEFINE(tt_log_backend, tt_log_api, true);

/* ----------------------------------------------------------- public */

int tt_init(void)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}
	tp_parser_init(&parser);
	k_work_init(&rx_restart_work, rx_restart);

	int err = uart_callback_set(uart, uart_cb, NULL);

	if (err) {
		return err;
	}
	err = uart_rx_enable(uart, rx_bufs[0], RX_BUF_BYTES, RX_IDLE_US);
	if (err) {
		return err;
	}

	k_thread_create(&tx_thread_data, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_thread,
			NULL, NULL, NULL, 10, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "telemetry_tx");
	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread,
			NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "telemetry_rx");
	return 0;
}

void tt_set_rx_callbacks(tt_rx_line_cb_t l, tt_rx_packet_cb_t p)
{
	line_cb = l;
	packet_cb = p;
}

void tt_set_binary_mode(bool on)
{
	atomic_set(&binary_mode, on ? 1 : 0);
}

bool tt_binary_mode(void)
{
	return atomic_get(&binary_mode) != 0;
}

int tt_send(const struct tp_header *h, const void *payload, size_t len)
{
	uint8_t pkt[TP_MAX_PACKET];
	size_t n = tp_encode(pkt, sizeof(pkt), h, payload, len);

	if (n == 0) {
		return -EINVAL;
	}
	return ring_put_atomic(pkt, n, h->type == TP_AUDIO_CHUNK);
}

static const char *type_tag(uint8_t type)
{
	switch (type) {
	case TP_HELLO: return "HELLO";
	case TP_RUN_START: return "RUN_START";
	case TP_RUN_CONFIG: return "RUN_CONFIG";
	case TP_AUDIO_GAP: return "AUDIO_GAP";
	case TP_AUDIO_STATS: return "AUDIO_STATS";
	case TP_WAKEWORD_EVENT: return "WAKEWORD_EVENT";
	case TP_PATH_METRICS: return "PATH_METRICS";
	case TP_RUN_END: return "RUN_END";
	case TP_ERROR: return "ERROR";
	case TP_LOG: return "LOG";
	case TP_ACK: return "ACK";
	case TP_STATUS: return "STATUS";
	case TP_REPLAY_ACK: return "REPLAY_ACK";
	case TP_TEXT: return "TEXT";
	default: return "?";
	}
}

int tt_send_text(uint8_t type, const char *fmt, ...)
{
	char line[240];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);

	va_end(ap);
	if (n < 0) {
		return -EINVAL;
	}
	if ((size_t)n >= sizeof(line)) {
		n = sizeof(line) - 1;
	}

	if (!tt_binary_mode()) {
		line[n++] = '\n';
		return ring_put_atomic((const uint8_t *)line, (size_t)n, false);
	}

	struct tp_header h = { .type = type, .flags = 0, .run_id = 0, .stream_id = (uint16_t)EVAL_STREAM_NONE,
			       .seq = 0, .timestamp = k_uptime_get_32(), .payload_len = 0 };

	return tt_send(&h, line, (size_t)n);
}

int tt_send_json(uint8_t type, uint16_t run_id, uint16_t stream_id, uint32_t seq,
		 uint32_t timestamp, uint16_t flags, const char *fmt, ...)
{
	char json[960];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(json, sizeof(json), fmt, ap);

	va_end(ap);
	if (n < 0) {
		return -EINVAL;
	}
	if ((size_t)n >= sizeof(json)) {
		return -EMSGSIZE;
	}

	if (!tt_binary_mode()) {
		char line[1000];
		int m = snprintf(line, sizeof(line), "%s %s\n", type_tag(type), json);

		return ring_put_atomic((const uint8_t *)line, (size_t)m, false);
	}

	struct tp_header h = { .type = type, .flags = flags, .run_id = run_id, .stream_id = stream_id,
			       .seq = seq, .timestamp = timestamp, .payload_len = 0 };

	return tt_send(&h, json, (size_t)n);
}

int tt_send_audio(uint16_t run_id, uint16_t stream_id, uint32_t seq, uint32_t sample_index,
		  uint16_t flags, const int16_t *pcm, size_t samples)
{
	if (!tt_binary_mode()) {
		return 0;
	}
	struct tp_header h = { .type = TP_AUDIO_CHUNK, .flags = flags, .run_id = run_id,
			       .stream_id = stream_id, .seq = seq, .timestamp = sample_index,
			       .payload_len = 0 };

	return tt_send(&h, pcm, samples * sizeof(int16_t));
}

void tt_get_stats(struct tt_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	*out = stats;
	k_spin_unlock(&tx_lock, key);
}

void tt_reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	memset(&stats, 0, sizeof(stats));
	stats.ring_size = CONFIG_EVAL_TX_RING_BYTES;
	k_spin_unlock(&tx_lock, key);
}

uint32_t tt_ring_used(void)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);
	uint32_t n = ring_buf_size_get(&tx_ring);

	k_spin_unlock(&tx_lock, key);
	return n;
}
