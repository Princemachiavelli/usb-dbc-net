/*
 * Stream framing shared by both ends of a dbcnet link.
 *
 * DbC gives us one bulk pipe per direction, and on a real target the bytes
 * pass through /dev/ttyDBC0, which keeps no transfer boundaries. Frames are
 * therefore self-delimiting in a byte stream: a 4-byte header (magic, then
 * payload length, both big-endian) followed by one IP packet. A receiver that
 * loses sync skips bytes until the magic reappears.
 */
#ifndef DBCNET_FRAME_H
#define DBCNET_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DBCNET_MAGIC 0xdb5e
#define DBCNET_HDR 4
#define DBCNET_MAX_PAYLOAD 65535

/* Senders pack queued packets into one bulk transfer until it reaches this. */
#define DBCNET_BATCH 32768
/* Bulk IN reads use this size; a multiple of the 1024-byte SS max packet. */
#define DBCNET_RX_CHUNK 32768

void dbcnet_put_header(uint8_t *dst, size_t payload_len);

typedef void (*dbcnet_frame_fn)(void *ctx, const uint8_t *payload, size_t len);

struct dbcnet_decoder {
	/* At most one partial frame is carried over, plus one new chunk. */
	uint8_t buf[DBCNET_HDR + DBCNET_MAX_PAYLOAD + DBCNET_RX_CHUNK];
	size_t len;
	uint64_t frames;
	uint64_t skipped; /* bytes dropped while resynchronising */
};

void dbcnet_decoder_init(struct dbcnet_decoder *d);
/* Feeds up to DBCNET_RX_CHUNK bytes; calls fn once per complete frame. */
int dbcnet_decode(struct dbcnet_decoder *d, const uint8_t *data, size_t len,
		  dbcnet_frame_fn fn, void *ctx);

struct dbcnet_batch {
	/* Filling stops at DBCNET_BATCH, so one more maximal frame always fits. */
	uint8_t buf[DBCNET_BATCH + DBCNET_HDR + DBCNET_MAX_PAYLOAD];
	size_t len;
};

/*
 * Waits up to timeout_ms for a packet on the non-blocking TUN fd, then drains
 * queued packets into b as frames until DBCNET_BATCH is reached. Returns the
 * batch length, 0 on timeout, or -1 with errno set.
 */
ssize_t dbcnet_fill_batch(int tun, struct dbcnet_batch *b, int timeout_ms);

/* Opens (creating if needed) a non-blocking IFF_TUN|IFF_NO_PI interface. */
int dbcnet_tun_open(const char *name);

#endif
