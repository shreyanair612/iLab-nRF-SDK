/*
 * Serial telemetry transport over uart20 (J-Link VCOM).
 *
 * Device -> host: a lock-protected transmit ring drained by a low-priority
 * thread using the async UART API, so audio capture and inference never wait
 * on the link. When the ring is full a packet is dropped and counted; audio
 * drops are reported to the host as AUDIO_GAP by the run controller.
 *
 * Host -> device: async UART receive into a byte ring, parsed by a small
 * thread. Framed packets (commands, replay audio) and plain text lines are
 * both accepted, so a terminal can drive the device without any tooling.
 *
 * Text vs binary mode: the device boots in text mode and prints readable
 * lines. The host tool sends "binary_mode on" and every outgoing message is
 * then framed (TP_LOG for Zephyr log lines, TP_TEXT for summaries). Zephyr's
 * logging is routed through this transport by a custom backend so log text
 * can never tear a binary packet apart.
 */
#ifndef TELEMETRY_TRANSPORT_H_
#define TELEMETRY_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetry_protocol.h"

struct tt_stats {
	uint32_t bytes_sent;
	uint32_t packets_sent;
	uint32_t packets_dropped;    /* ring full                             */
	uint32_t audio_dropped;      /* subset of packets_dropped that were audio */
	uint32_t ring_high_water;
	uint32_t ring_size;
	uint32_t rx_bytes;
	uint32_t rx_packets;
	uint32_t rx_bad_crc;
	uint32_t rx_resyncs;
	uint32_t rx_overflow;
	uint32_t log_lines_dropped;
};

typedef void (*tt_rx_line_cb_t)(const char *line);
typedef void (*tt_rx_packet_cb_t)(const struct tp_header *h, const uint8_t *payload);

int tt_init(void);
void tt_set_rx_callbacks(tt_rx_line_cb_t line_cb, tt_rx_packet_cb_t packet_cb);

void tt_set_binary_mode(bool on);
bool tt_binary_mode(void);

/* Framed send. Returns 0, or -ENOSPC if the ring cannot take the whole packet. */
int tt_send(const struct tp_header *h, const void *payload, size_t len);

/* Text-type packet (TP_LOG, TP_ACK, TP_TEXT, TP_ERROR). In text mode the
 * line is written raw. printf-style; the line must fit in 240 bytes. */
int tt_send_text(uint8_t type, const char *fmt, ...);

/* JSON-type packet. printf-style; payload must fit in 512 bytes. In text mode
 * the payload is written as "<TYPE> {json}". */
int tt_send_json(uint8_t type, uint16_t run_id, uint16_t stream_id, uint32_t seq,
		 uint32_t timestamp, uint16_t flags, const char *fmt, ...);

/* Audio chunk. Silently discarded in text mode. */
int tt_send_audio(uint16_t run_id, uint16_t stream_id, uint32_t seq, uint32_t sample_index,
		  uint16_t flags, const int16_t *pcm, size_t samples);

void tt_get_stats(struct tt_stats *out);
void tt_reset_stats(void);

/* Bytes currently queued, for queue-watermark telemetry. */
uint32_t tt_ring_used(void);

#endif /* TELEMETRY_TRANSPORT_H_ */
