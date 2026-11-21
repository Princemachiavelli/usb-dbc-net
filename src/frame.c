#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void dbcnet_put_header(uint8_t *dst, size_t payload_len)
{
	dst[0] = DBCNET_MAGIC >> 8;
	dst[1] = DBCNET_MAGIC & 0xff;
	dst[2] = payload_len >> 8;
	dst[3] = payload_len & 0xff;
}

void dbcnet_decoder_init(struct dbcnet_decoder *d)
{
	memset(d, 0, sizeof(*d));
}

int dbcnet_decode(struct dbcnet_decoder *d, const uint8_t *data, size_t len,
		  dbcnet_frame_fn fn, void *ctx)
{
	if (len > sizeof(d->buf) - d->len) {
		errno = EMSGSIZE;
		return -1;
	}
	memcpy(d->buf + d->len, data, len);
	d->len += len;

	size_t pos = 0;
	while (d->len - pos >= DBCNET_HDR) {
		const uint8_t *h = d->buf + pos;
		if (((h[0] << 8) | h[1]) != DBCNET_MAGIC) {
			pos++;
			d->skipped++;
			continue;
		}
		size_t plen = (h[2] << 8) | h[3];
		if (d->len - pos < DBCNET_HDR + plen)
			break;
		/* A zero-length frame carries nothing; treat it as padding. */
		if (plen) {
			fn(ctx, h + DBCNET_HDR, plen);
			d->frames++;
		}
		pos += DBCNET_HDR + plen;
	}
	memmove(d->buf, d->buf + pos, d->len - pos);
	d->len -= pos;
	return 0;
}

ssize_t dbcnet_fill_batch(int tun, struct dbcnet_batch *b, int timeout_ms)
{
	struct pollfd pfd = { .fd = tun, .events = POLLIN };
	int r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return r;

	b->len = 0;
	while (b->len < DBCNET_BATCH) {
		ssize_t n = read(tun, b->buf + b->len + DBCNET_HDR,
				 DBCNET_MAX_PAYLOAD);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				break;
			return -1;
		}
		if (n == 0)
			break;
		dbcnet_put_header(b->buf + b->len, n);
		b->len += DBCNET_HDR + n;
	}
	return b->len;
}

int dbcnet_tun_open(const char *name)
{
	int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct ifreq ifr = { .ifr_flags = IFF_TUN | IFF_NO_PI };
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}
