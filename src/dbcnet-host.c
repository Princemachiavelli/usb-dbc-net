/*
 * dbcnet-host: the debug-host end of a dbcnet link, in userspace. Finds the
 * DbC device by VID:PID, takes its bulk endpoints from usb_debug through
 * libusb, and bridges them to a TUN interface with the dbcnet stream framing.
 * The TUN interface outlives the device: when it goes away, we wait for it to
 * come back.
 */
#include "frame.h"

#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POLL_MS 200
#define OUT_TIMEOUT_MS 2000

static uint16_t opt_vid = 0x1d6b;
static uint16_t opt_pid = 0x0010;
static const char *opt_tun = "dbc0";
static bool opt_zlp = true;

static libusb_context *ctx;
static int tun;

struct session {
	libusb_device_handle *h;
	uint8_t ep_in, ep_out;
	atomic_bool dead;
};

static void write_frame(void *arg, const uint8_t *payload, size_t len)
{
	(void)arg;
	if (write(tun, payload, len) < 0)
		fprintf(stderr, "dbcnet-host: tun write: %s\n", strerror(errno));
}

/* Device -> host: bulk IN stream -> frames -> TUN. */
static void *rx_thread(void *arg)
{
	struct session *s = arg;
	static struct dbcnet_decoder dec;
	static uint8_t buf[DBCNET_RX_CHUNK];
	dbcnet_decoder_init(&dec);
	while (!s->dead) {
		int got = 0;
		int r = libusb_bulk_transfer(s->h, s->ep_in, buf, sizeof(buf),
					     &got, POLL_MS);
		/* A timed-out transfer may still have moved data. */
		if (got > 0 && dbcnet_decode(&dec, buf, got, write_frame, NULL) < 0)
			fprintf(stderr, "dbcnet-host: decode: %s\n", strerror(errno));
		if (r == 0 || r == LIBUSB_ERROR_TIMEOUT)
			continue;
		fprintf(stderr, "dbcnet-host: bulk in: %s\n", libusb_error_name(r));
		s->dead = true;
	}
	return NULL;
}

static void LIBUSB_CALL out_done(struct libusb_transfer *t)
{
	*(int *)t->user_data = 1;
}

/*
 * libusb's synchronous API cannot ask for a zero-length packet, which a
 * transfer ending on a packet boundary needs so the device's larger read
 * completes; so this is the synchronous call written out with the flag.
 */
static int bulk_out(struct session *s, uint8_t *data, int len)
{
	struct libusb_transfer *t = libusb_alloc_transfer(0);
	int done = 0;
	libusb_fill_bulk_transfer(t, s->h, s->ep_out, data, len, out_done,
				  &done, OUT_TIMEOUT_MS);
	if (opt_zlp)
		t->flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
	int r = libusb_submit_transfer(t);
	if (r < 0) {
		libusb_free_transfer(t);
		return r;
	}
	while (!done) {
		r = libusb_handle_events_completed(ctx, &done);
		if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
			libusb_cancel_transfer(t);
			while (!done)
				libusb_handle_events_completed(ctx, &done);
			break;
		}
	}
	switch (t->status) {
	case LIBUSB_TRANSFER_COMPLETED: r = 0; break;
	case LIBUSB_TRANSFER_TIMED_OUT: r = LIBUSB_ERROR_TIMEOUT; break;
	case LIBUSB_TRANSFER_NO_DEVICE: r = LIBUSB_ERROR_NO_DEVICE; break;
	case LIBUSB_TRANSFER_STALL: r = LIBUSB_ERROR_PIPE; break;
	default: r = LIBUSB_ERROR_IO; break;
	}
	libusb_free_transfer(t);
	return r;
}

/* Host -> device: TUN -> batched frames -> bulk OUT. */
static void *tx_thread(void *arg)
{
	struct session *s = arg;
	static struct dbcnet_batch batch;
	while (!s->dead) {
		ssize_t n = dbcnet_fill_batch(tun, &batch, POLL_MS);
		if (n < 0) {
			fprintf(stderr, "dbcnet-host: tun read: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (n == 0)
			continue;
		int r = bulk_out(s, batch.buf, n);
		if (r == LIBUSB_ERROR_TIMEOUT) {
			/* The receiver resynchronises if part of the batch went. */
			fprintf(stderr, "dbcnet-host: bulk out timed out\n");
		} else if (r < 0) {
			fprintf(stderr, "dbcnet-host: bulk out: %s\n",
				libusb_error_name(r));
			s->dead = true;
		}
	}
	return NULL;
}

static bool find_endpoints(struct session *s)
{
	struct libusb_config_descriptor *cfg;
	if (libusb_get_active_config_descriptor(libusb_get_device(s->h), &cfg))
		return false;
	s->ep_in = s->ep_out = 0;
	const struct libusb_interface_descriptor *alt = &cfg->interface[0].altsetting[0];
	for (int i = 0; i < alt->bNumEndpoints; i++) {
		const struct libusb_endpoint_descriptor *ep = &alt->endpoint[i];
		if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
		    LIBUSB_TRANSFER_TYPE_BULK)
			continue;
		if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
			s->ep_in = s->ep_in ? s->ep_in : ep->bEndpointAddress;
		else
			s->ep_out = s->ep_out ? s->ep_out : ep->bEndpointAddress;
	}
	libusb_free_config_descriptor(cfg);
	return s->ep_in && s->ep_out;
}

static void run_session(libusb_device_handle *h)
{
	struct session s = { .h = h };
	libusb_set_auto_detach_kernel_driver(h, 1);
	int r = libusb_claim_interface(h, 0);
	if (r < 0) {
		fprintf(stderr, "dbcnet-host: claim: %s\n", libusb_error_name(r));
		return;
	}
	if (!find_endpoints(&s)) {
		fprintf(stderr, "dbcnet-host: no bulk endpoint pair\n");
		libusb_release_interface(h, 0);
		return;
	}
	fprintf(stderr, "dbcnet-host: connected, in 0x%02x out 0x%02x\n",
		s.ep_in, s.ep_out);

	pthread_t rx, tx;
	pthread_create(&rx, NULL, rx_thread, &s);
	pthread_create(&tx, NULL, tx_thread, &s);
	pthread_join(rx, NULL);
	s.dead = true;
	pthread_join(tx, NULL);
	libusb_release_interface(h, 0);
	fprintf(stderr, "dbcnet-host: disconnected\n");
}

static void usage(void)
{
	fprintf(stderr, "usage: dbcnet-host [--tun NAME] [--vid HEX] [--pid HEX] [--no-zlp]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "tun", required_argument, NULL, 't' },
		{ "vid", required_argument, NULL, 'v' },
		{ "pid", required_argument, NULL, 'p' },
		{ "no-zlp", no_argument, NULL, 'z' },
		{ 0 },
	};
	int o;
	while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (o) {
		case 't': opt_tun = optarg; break;
		case 'v': opt_vid = strtoul(optarg, NULL, 16); break;
		case 'p': opt_pid = strtoul(optarg, NULL, 16); break;
		case 'z': opt_zlp = false; break;
		default: usage();
		}
	}

	tun = dbcnet_tun_open(opt_tun);
	if (tun < 0) {
		fprintf(stderr, "dbcnet-host: open tun %s: %s\n", opt_tun,
			strerror(errno));
		return EXIT_FAILURE;
	}
	int r = libusb_init_context(&ctx, NULL, 0);
	if (r < 0) {
		fprintf(stderr, "dbcnet-host: libusb: %s\n", libusb_error_name(r));
		return EXIT_FAILURE;
	}
	fprintf(stderr, "dbcnet-host: waiting for %04x:%04x\n", opt_vid, opt_pid);
	for (;;) {
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, opt_vid, opt_pid);
		if (!h) {
			usleep(POLL_MS * 1000);
			continue;
		}
		run_session(h);
		libusb_close(h);
	}
}
