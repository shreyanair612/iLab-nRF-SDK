/*
 * Framed binary telemetry protocol, shared by firmware and tools/serial_protocol.py.
 *
 * Wire format, little-endian:
 *
 *   offset  size  field
 *   0       2     magic        0xA5 0x5A
 *   2       1     version      TP_VERSION
 *   3       1     type         tp_type_t
 *   4       2     flags        tp_flags
 *   6       2     run_id       0 when no run is active
 *   8       2     stream_id    eval_stream_id_t / path id, 0xFFFF if n/a
 *   10      4     seq          per-stream sequence for AUDIO_CHUNK, global otherwise
 *   14      4     timestamp    audio sample index for audio/events, uptime ms otherwise
 *   18      2     payload_len  0..TP_MAX_PAYLOAD
 *   20      n     payload
 *   20+n    2     crc16        CRC-16/CCITT-FALSE over bytes [2 .. 20+n)
 *
 * Host -> device packets use the same framing. A plain text line (ending in
 * '\n') that does not start with the magic is accepted as a command, so a
 * human at a terminal can type commands without any tooling.
 *
 * Pure C, no Zephyr dependency.
 */
#ifndef TELEMETRY_PROTOCOL_H_
#define TELEMETRY_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TP_MAGIC0       0xA5U
#define TP_MAGIC1       0x5AU
#define TP_VERSION      1U
#define TP_HEADER_LEN   20U
#define TP_CRC_LEN      2U
#define TP_MAX_PAYLOAD  1024U
#define TP_MAX_PACKET   (TP_HEADER_LEN + TP_MAX_PAYLOAD + TP_CRC_LEN)

typedef enum {
	/* device -> host */
	TP_HELLO = 0x01,          /* JSON: device identity and capabilities        */
	TP_RUN_START = 0x02,      /* JSON: run id and config snapshot              */
	TP_RUN_CONFIG = 0x03,     /* JSON: full config (also on request)           */
	TP_AUDIO_CHUNK = 0x04,    /* int16 PCM, stream_id selects the stream       */
	TP_AUDIO_GAP = 0x05,      /* JSON: stream, frames lost, reason             */
	TP_AUDIO_STATS = 0x06,    /* JSON: per-stream rms/peak/clips for the window */
	TP_WAKEWORD_EVENT = 0x07, /* JSON: path, sample index, score               */
	TP_PATH_METRICS = 0x08,   /* JSON: per-path counters                       */
	TP_RUN_END = 0x09,        /* JSON: final summary                           */
	TP_ERROR = 0x0A,          /* text                                          */
	TP_LOG = 0x0B,            /* text: one log line                            */
	TP_ACK = 0x0C,            /* text: command echo and result                 */
	TP_STATUS = 0x0D,         /* JSON: live health                             */
	TP_REPLAY_ACK = 0x0E,     /* JSON: frames consumed, credits                */
	TP_TEXT = 0x0F,           /* text: human summary lines                     */

	/* host -> device */
	TP_COMMAND = 0x40,        /* text: one command line                        */
	TP_REPLAY_AUDIO = 0x41,   /* int16 interleaved L,R[,C] frames              */
	TP_REPLAY_END = 0x42,     /* empty                                         */
} tp_type_t;

enum tp_flags {
	TP_FLAG_INCOMPLETE = 1U << 0,  /* payload truncated                        */
	TP_FLAG_FINAL = 1U << 1,       /* last packet of a series                  */
	TP_FLAG_REPLAY = 1U << 2,      /* produced during a replay pass            */
};

struct tp_header {
	uint8_t type;
	uint16_t flags;
	uint16_t run_id;
	uint16_t stream_id;
	uint32_t seq;
	uint32_t timestamp;
	uint16_t payload_len;
};

uint16_t tp_crc16(uint16_t seed, const uint8_t *data, size_t len);

/*
 * Serialises header + payload + crc into out. Returns total bytes written, or
 * 0 if out_cap is too small or payload_len exceeds TP_MAX_PAYLOAD.
 */
size_t tp_encode(uint8_t *out, size_t out_cap, const struct tp_header *h,
		 const void *payload, size_t payload_len);

/* Streaming parser for the device's receive side. */
typedef enum {
	TP_PARSE_NONE = 0,
	TP_PARSE_PACKET,     /* a valid framed packet is available                */
	TP_PARSE_TEXT_LINE,  /* a plain text line (without '\n') is available     */
	TP_PARSE_BAD_CRC,
} tp_parse_result_t;

struct tp_parser {
	uint8_t buf[TP_MAX_PACKET];
	size_t len;
	size_t expected;       /* total packet length once header is known */
	bool in_packet;
	struct tp_header hdr;
	uint32_t bad_crc;
	uint32_t resyncs;
};

void tp_parser_init(struct tp_parser *p);

/*
 * Feed one byte. On TP_PARSE_PACKET, p->hdr is filled and the payload is at
 * p->buf + TP_HEADER_LEN. On TP_PARSE_TEXT_LINE, the NUL-terminated line is
 * at p->buf (length p->len). The parser resets itself after either result.
 */
tp_parse_result_t tp_parser_feed(struct tp_parser *p, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_PROTOCOL_H_ */
