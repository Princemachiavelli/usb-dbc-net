/*
 * dbcnet-tty: the target end of a dbcnet link over the kernel's DbC tty
 * (/dev/ttyDBC0, CONFIG_USB_XHCI_DBGCAP), bridged to a TUN interface with the
 * dbcnet stream framing. The tty exists only while a debug host has the DbC
 * configured, so we wait for it and reopen it after each disconnect; the TUN
 * interface stays.
 *
 * DbC never sends a zero-length packet, and ttyDBC queues what we write in
 * TRBs of up to 1024 bytes. A batch ending on a packet boundary would not end
 * the debug host's larger bulk read, which would then wait for more data or
 * its timeout. Such a batch gets a zero-length frame appended, so its last
 * packet is short.
 */
#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define POLL_MS 200
#define DBC_MAX_PACKET 1024

static const char *opt_tty = "/dev/ttyDBC0";
static const char *opt_tun = "dbc0";
static bool opt_pad = true;

static int tun;

struct session {
	int tty;
	atomic_bool dead;
};

static void write_frame(void *arg, const uint8_t *payload, size_t len)
{
	(void)arg;
	if (write(tun, payload, len) < 0)
		fprintf(stderr, "dbcnet-tty: tun write: %s\n", strerror(errno));
}

/* Debug host -> target: tty byte stream -> frames -> TUN. */
static void *rx_thread(void *arg)
{
	struct session *s = arg;
	static struct dbcnet_decoder dec;
	static uint8_t buf[DBCNET_RX_CHUNK];
	dbcnet_decoder_init(&dec);
	while (!s->dead) {
		struct pollfd pfd = { .fd = s->tty, .events = POLLIN };
		int r = poll(&pfd, 1, POLL_MS);
		if (r == 0 || (r < 0 && errno == EINTR))
			continue;
		ssize_t n = r < 0 ? -1 : read(s->tty, buf, sizeof(buf));
		if (n <= 0) {
			fprintf(stderr, "dbcnet-tty: tty read: %s\n",
				n ? strerror(errno) : "hangup");
			s->dead = true;
			break;
		}
		if (dbcnet_decode(&dec, buf, n, write_frame, NULL) < 0)
			fprintf(stderr, "dbcnet-tty: decode: %s\n", strerror(errno));
	}
	return NULL;
}

static bool write_all(int fd, const uint8_t *p, size_t len)
{
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

/* Target -> debug host: TUN -> batched frames -> tty. */
static void *tx_thread(void *arg)
{
	struct session *s = arg;
	static struct dbcnet_batch batch;
	while (!s->dead) {
		ssize_t n = dbcnet_fill_batch(tun, &batch, POLL_MS);
		if (n < 0) {
			fprintf(stderr, "dbcnet-tty: tun read: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (n == 0)
			continue;
		if (opt_pad && n % DBC_MAX_PACKET == 0) {
			dbcnet_put_header(batch.buf + n, 0);
			n += DBCNET_HDR;
		}
		if (!write_all(s->tty, batch.buf, n)) {
			fprintf(stderr, "dbcnet-tty: tty write: %s\n", strerror(errno));
			s->dead = true;
		}
	}
	return NULL;
}

static int tty_open(void)
{
	int fd = open(opt_tty, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &t);
	}
	return fd;
}

static void usage(void)
{
	fprintf(stderr, "usage: dbcnet-tty [--tty PATH] [--tun NAME] [--no-pad]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "tty", required_argument, NULL, 'y' },
		{ "tun", required_argument, NULL, 't' },
		{ "no-pad", no_argument, NULL, 'n' },
		{ 0 },
	};
	int o;
	while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (o) {
		case 'y': opt_tty = optarg; break;
		case 't': opt_tun = optarg; break;
		case 'n': opt_pad = false; break;
		default: usage();
		}
	}

	tun = dbcnet_tun_open(opt_tun);
	if (tun < 0) {
		fprintf(stderr, "dbcnet-tty: open tun %s: %s\n", opt_tun,
			strerror(errno));
		return EXIT_FAILURE;
	}
	fprintf(stderr, "dbcnet-tty: waiting for %s\n", opt_tty);
	for (;;) {
		int fd = tty_open();
		if (fd < 0) {
			usleep(POLL_MS * 1000);
			continue;
		}
		fprintf(stderr, "dbcnet-tty: connected\n");
		struct session s = { .tty = fd };
		pthread_t rx, tx;
		pthread_create(&rx, NULL, rx_thread, &s);
		pthread_create(&tx, NULL, tx_thread, &s);
		pthread_join(rx, NULL);
		s.dead = true;
		pthread_join(tx, NULL);
		close(fd);
		fprintf(stderr, "dbcnet-tty: disconnected\n");
	}
}
