#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "eval_config.h"
#include "telemetry_transport.h"

LOG_MODULE_REGISTER(telemetry, LOG_LEVEL_INF);

#ifndef CONFIG_EVAL_TX_RING_BYTES
#define CONFIG_EVAL_TX_RING_BYTES 65536
#endif
#ifndef CONFIG_EVAL_TEXT_MODE_AT_BOOT
#define CONFIG_EVAL_TEXT_MODE_AT_BOOT 1
#endif

#define CDC_NODE        DT_NODELABEL(cdc_acm_uart0)
#define TX_CHUNK_BYTES  512
#define RX_CHUNK_BYTES  64
#define RX_RING_BYTES   4096

static const struct device *const cdc = DEVICE_DT_GET(CDC_NODE);

/* ------------------------------------------------------------ TX side */

RING_BUF_DECLARE(tx_ring, CONFIG_EVAL_TX_RING_BYTES);
static struct k_spinlock tx_lock;
static atomic_t binary_mode = ATOMIC_INIT(CONFIG_EVAL_TEXT_MODE_AT_BOOT ? 0 : 1);
static struct tt_stats stats = { .ring_size = CONFIG_EVAL_TX_RING_BYTES };

/* All-or-nothing enqueue. Safe from any thread. */
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
		uart_irq_tx_enable(cdc);
	}
	return rc;
}

/* Called from the UART callback while the class has room in its TX FIFO. */
static void tx_drain(void)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);
	uint8_t *data;
	uint32_t n = ring_buf_get_claim(&tx_ring, &data, TX_CHUNK_BYTES);

	if (n == 0) {
		k_spin_unlock(&tx_lock, key);
		uart_irq_tx_disable(cdc);
		return;
	}

	int sent = uart_fifo_fill(cdc, data, (int)n);

	ring_buf_get_finish(&tx_ring, sent > 0 ? (uint32_t)sent : 0U);
	k_spin_unlock(&tx_lock, key);
}

/* ------------------------------------------------------------ RX side */

RING_BUF_DECLARE(rx_ring, RX_RING_BYTES);
static K_SEM_DEFINE(rx_sem, 0, 1);
static tt_rx_line_cb_t line_cb;
static tt_rx_packet_cb_t packet_cb;
static struct tp_parser parser;

K_THREAD_STACK_DEFINE(rx_stack, 2048);
static struct k_thread rx_thread_data;

static void uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[RX_CHUNK_BYTES];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				uint32_t put = ring_buf_put(&rx_ring, buf, (uint32_t)n);

				if (put < (uint32_t)n) {
					stats.rx_overflow += (uint32_t)n - put;
				}
				stats.rx_bytes += put;
				k_sem_give(&rx_sem);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			tx_drain();
		}
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

/* ----------------------------------------------------------- public */

int tt_init(void)
{
	if (!device_is_ready(cdc)) {
		return -ENODEV;
	}
	tp_parser_init(&parser);

	int err = uart_irq_callback_user_data_set(cdc, uart_cb, NULL);

	if (err) {
		return err;
	}
	uart_irq_rx_enable(cdc);

	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread,
			NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "telemetry_rx");

	LOG_INF("telemetry on USB CDC ACM, tx ring %u bytes", (unsigned)CONFIG_EVAL_TX_RING_BYTES);
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

bool tt_host_connected(void)
{
	uint32_t dtr = 0;

	if (uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) != 0) {
		return false;
	}
	return dtr != 0;
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
	case TP_RUN_END: return "RUN_END";
	case TP_ERROR: return "ERROR";
	case TP_LOG: return "LOG";
	case TP_ACK: return "ACK";
	case TP_STATUS: return "STATUS";
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

	struct tp_header h = { .type = type, .flags = 0, .run_id = 0,
			       .stream_id = (uint16_t)EVAL_STREAM_NONE, .seq = 0,
			       .timestamp = k_uptime_get_32(), .payload_len = 0 };

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
