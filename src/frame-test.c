/* Unit tests for the stream framing: split, batched and corrupted input. */
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

struct seen {
	size_t count;
	size_t lens[16];
	uint8_t first[16];
};

static void record(void *ctx, const uint8_t *p, size_t len)
{
	struct seen *s = ctx;
	if (s->count < 16) {
		s->lens[s->count] = len;
		s->first[s->count] = p[0];
	}
	s->count++;
}

static size_t frame(uint8_t *dst, size_t len, uint8_t fill)
{
	dbcnet_put_header(dst, len);
	memset(dst + DBCNET_HDR, fill, len);
	return DBCNET_HDR + len;
}

static struct dbcnet_decoder dec;

static void test_whole_frames_in_one_chunk(void)
{
	uint8_t buf[4096];
	size_t n = frame(buf, 100, 0xa1);
	n += frame(buf + n, 1020, 0xa2);
	n += frame(buf + n, 1, 0xa3);
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	CHECK(dbcnet_decode(&dec, buf, n, record, &s) == 0);
	CHECK(s.count == 3);
	CHECK(s.lens[0] == 100 && s.first[0] == 0xa1);
	CHECK(s.lens[1] == 1020 && s.first[1] == 0xa2);
	CHECK(s.lens[2] == 1 && s.first[2] == 0xa3);
	CHECK(dec.len == 0 && dec.skipped == 0);
}

static void test_byte_at_a_time(void)
{
	uint8_t buf[4096];
	size_t n = frame(buf, 1500, 0xb1);
	n += frame(buf + n, 9, 0xb2);
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	for (size_t i = 0; i < n; i++)
		CHECK(dbcnet_decode(&dec, buf + i, 1, record, &s) == 0);
	CHECK(s.count == 2);
	CHECK(s.lens[0] == 1500 && s.lens[1] == 9);
}

static void test_resync_after_garbage(void)
{
	uint8_t buf[4096] = { 0x00, 0xdb, 0x12, 0xdb };
	size_t n = 4;
	n += frame(buf + n, 64, 0xc1);
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	CHECK(dbcnet_decode(&dec, buf, n, record, &s) == 0);
	CHECK(s.count == 1 && s.lens[0] == 64 && s.first[0] == 0xc1);
	CHECK(dec.skipped == 4);
}

static void test_max_payload_split(void)
{
	static uint8_t buf[DBCNET_HDR + DBCNET_MAX_PAYLOAD];
	size_t n = frame(buf, DBCNET_MAX_PAYLOAD, 0xd1);
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	for (size_t off = 0; off < n; off += DBCNET_RX_CHUNK) {
		size_t len = n - off < DBCNET_RX_CHUNK ? n - off : DBCNET_RX_CHUNK;
		CHECK(dbcnet_decode(&dec, buf + off, len, record, &s) == 0);
	}
	CHECK(s.count == 1 && s.lens[0] == DBCNET_MAX_PAYLOAD);
}

static void test_zero_length_is_padding(void)
{
	uint8_t buf[64];
	size_t n = frame(buf, 0, 0);
	n += frame(buf + n, 3, 0xe1);
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	CHECK(dbcnet_decode(&dec, buf, n, record, &s) == 0);
	CHECK(s.count == 1 && s.lens[0] == 3);
}

static void test_oversized_chunk_rejected(void)
{
	static uint8_t big[sizeof(dec.buf) + 1];
	struct seen s = { 0 };
	dbcnet_decoder_init(&dec);
	CHECK(dbcnet_decode(&dec, big, sizeof(big), record, &s) == -1);
}

int main(void)
{
	test_whole_frames_in_one_chunk();
	test_byte_at_a_time();
	test_resync_after_garbage();
	test_max_payload_split();
	test_zero_length_is_padding();
	test_oversized_chunk_rejected();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("frame-test: ok");
	return EXIT_SUCCESS;
}
