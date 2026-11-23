/*
 * dbc-impostor: a raw_gadget USB device shaped like the xHCI Debug Capability,
 * standing in for a DbC target where no DbC hardware exists (dummy_hcd in a
 * VM test). It presents DbC's identity and its single interface with one bulk
 * OUT and one bulk IN endpoint (EP1, 1024-byte SuperSpeed packets), and bridges
 * them to a TUN interface with the dbcnet stream framing.
 *
 * Interface class/subclass 0xdc/0x02 follow the USB debug class as we
 * understand it; the host side matches on VID:PID and endpoints only.
 */
#include "frame.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EP_OUT_ADDR (USB_DIR_OUT | 1)
#define EP_IN_ADDR (USB_DIR_IN | 1)
#define SS_MAXPACKET 1024
#define HS_MAXPACKET 512
#define EP0_MAX 512

/* DbC's defaults (drivers/usb/host/xhci-dbgcap.h). */
static uint16_t opt_vid = 0x1d6b;
static uint16_t opt_pid = 0x0010;
static uint8_t opt_protocol = 1;
static const char *opt_tun = "dbc0";
static const char *opt_udc_driver = "dummy_udc";
static const char *opt_udc_device = "dummy_udc.0";
static bool opt_zlp = true;
static bool opt_high_speed;

static uint16_t bulk_maxpacket(void)
{
	return opt_high_speed ? HS_MAXPACKET : SS_MAXPACKET;
}

static const char *const strings[] = {
	NULL, "Linux Foundation", "Linux USB Debug Target", "0001",
};

static int fd;
static int tun;
static int ep_out = -1, ep_in = -1;
static atomic_bool data_started;

#define die(...)                                                    \
	do {                                                        \
		fprintf(stderr, "dbc-impostor: " __VA_ARGS__);      \
		fprintf(stderr, ": %s\n", strerror(errno));         \
		exit(EXIT_FAILURE);                                 \
	} while (0)

static struct usb_device_descriptor device_desc(void)
{
	return (struct usb_device_descriptor){
		.bLength = USB_DT_DEVICE_SIZE,
		.bDescriptorType = USB_DT_DEVICE,
		.bcdUSB = htole16(opt_high_speed ? 0x0200 : 0x0300),
		.bMaxPacketSize0 = opt_high_speed ? 64 : 9, /* SuperSpeed: 2^9 */
		.idVendor = htole16(opt_vid),
		.idProduct = htole16(opt_pid),
		.bcdDevice = htole16(0x0010),
		.iManufacturer = 1,
		.iProduct = 2,
		.iSerialNumber = 3,
		.bNumConfigurations = 1,
	};
}

static size_t config_desc(uint8_t *out)
{
	struct usb_config_descriptor cfg = {
		.bLength = USB_DT_CONFIG_SIZE,
		.bDescriptorType = USB_DT_CONFIG,
		.bNumInterfaces = 1,
		.bConfigurationValue = 1,
		.bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
		.bMaxPower = 0,
	};
	struct usb_interface_descriptor intf = {
		.bLength = USB_DT_INTERFACE_SIZE,
		.bDescriptorType = USB_DT_INTERFACE,
		.bNumEndpoints = 2,
		.bInterfaceClass = 0xdc,
		.bInterfaceSubClass = 0x02,
		.bInterfaceProtocol = opt_protocol,
	};
	struct usb_endpoint_descriptor eps[2] = {
		{
			.bLength = USB_DT_ENDPOINT_SIZE,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = EP_OUT_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = htole16(bulk_maxpacket()),
		},
		{
			.bLength = USB_DT_ENDPOINT_SIZE,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = EP_IN_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = htole16(bulk_maxpacket()),
		},
	};
	struct usb_ss_ep_comp_descriptor comp = {
		.bLength = USB_DT_SS_EP_COMP_SIZE,
		.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	};

	size_t n = 0;
	memcpy(out + n, &cfg, USB_DT_CONFIG_SIZE);
	n += USB_DT_CONFIG_SIZE;
	memcpy(out + n, &intf, USB_DT_INTERFACE_SIZE);
	n += USB_DT_INTERFACE_SIZE;
	for (int i = 0; i < 2; i++) {
		memcpy(out + n, &eps[i], USB_DT_ENDPOINT_SIZE);
		n += USB_DT_ENDPOINT_SIZE;
		if (opt_high_speed)
			continue;
		memcpy(out + n, &comp, USB_DT_SS_EP_COMP_SIZE);
		n += USB_DT_SS_EP_COMP_SIZE;
	}
	uint16_t total = htole16(n);
	memcpy(out + offsetof(struct usb_config_descriptor, wTotalLength),
	       &total, sizeof(total));
	return n;
}

static size_t bos_desc(uint8_t *out)
{
	struct usb_ext_cap_descriptor ext = {
		.bLength = USB_DT_USB_EXT_CAP_SIZE,
		.bDescriptorType = USB_DT_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_CAP_TYPE_EXT,
	};
	struct usb_ss_cap_descriptor ss = {
		.bLength = USB_DT_USB_SS_CAP_SIZE,
		.bDescriptorType = USB_DT_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_SS_CAP_TYPE,
		.wSpeedSupported = htole16(USB_5GBPS_OPERATION),
		.bFunctionalitySupport = 3, /* lowest speed with full function: SS */
	};
	struct usb_bos_descriptor bos = {
		.bLength = USB_DT_BOS_SIZE,
		.bDescriptorType = USB_DT_BOS,
		.wTotalLength = htole16(USB_DT_BOS_SIZE + USB_DT_USB_EXT_CAP_SIZE +
					USB_DT_USB_SS_CAP_SIZE),
		.bNumDeviceCaps = 2,
	};
	size_t n = 0;
	memcpy(out + n, &bos, USB_DT_BOS_SIZE);
	n += USB_DT_BOS_SIZE;
	memcpy(out + n, &ext, USB_DT_USB_EXT_CAP_SIZE);
	n += USB_DT_USB_EXT_CAP_SIZE;
	memcpy(out + n, &ss, USB_DT_USB_SS_CAP_SIZE);
	n += USB_DT_USB_SS_CAP_SIZE;
	return n;
}

static size_t string_desc(uint8_t index, uint8_t *out)
{
	if (index == 0) {
		const uint8_t langs[] = { 4, USB_DT_STRING, 0x09, 0x04 };
		memcpy(out, langs, sizeof(langs));
		return sizeof(langs);
	}
	if (index >= sizeof(strings) / sizeof(strings[0]))
		return 0;
	size_t n = 2;
	for (const char *c = strings[index]; *c; c++) {
		out[n++] = *c;
		out[n++] = 0;
	}
	out[0] = n;
	out[1] = USB_DT_STRING;
	return n;
}

static void ep0_write(const void *data, size_t len, uint16_t wlength)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + EP0_MAX);
	if (len > wlength)
		len = wlength;
	io->ep = 0;
	io->flags = 0;
	io->length = len;
	memcpy(io->data, data, len);
	if (ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io) < 0)
		fprintf(stderr, "dbc-impostor: ep0 write: %s\n", strerror(errno));
	free(io);
}

/* Completes an OUT control transfer: reads its data stage, if any, and acks. */
static void ep0_read(uint16_t wlength)
{
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + EP0_MAX);
	io->ep = 0;
	io->flags = 0;
	io->length = wlength < EP0_MAX ? wlength : EP0_MAX;
	if (ioctl(fd, USB_RAW_IOCTL_EP0_READ, io) < 0)
		fprintf(stderr, "dbc-impostor: ep0 read: %s\n", strerror(errno));
	free(io);
}

static void ep0_stall(void)
{
	if (ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0) < 0)
		fprintf(stderr, "dbc-impostor: ep0 stall: %s\n", strerror(errno));
}

static int ep_enable(uint8_t addr)
{
	struct usb_endpoint_descriptor d = {
		.bLength = USB_DT_ENDPOINT_SIZE,
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = addr,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
		.wMaxPacketSize = htole16(bulk_maxpacket()),
	};
	int h = ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, &d);
	if (h < 0)
		die("enable endpoint 0x%02x", addr);
	return h;
}

static void write_frame(void *ctx, const uint8_t *payload, size_t len)
{
	(void)ctx;
	if (write(tun, payload, len) < 0)
		fprintf(stderr, "dbc-impostor: tun write: %s\n", strerror(errno));
}

/* Host -> target: bulk OUT stream -> frames -> TUN. */
static void *out_thread(void *arg)
{
	(void)arg;
	static struct dbcnet_decoder dec;
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + DBCNET_RX_CHUNK);
	dbcnet_decoder_init(&dec);
	for (;;) {
		io->ep = ep_out;
		io->flags = 0;
		io->length = DBCNET_RX_CHUNK;
		int n = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
		if (n < 0) {
			/* Bus reset or disconnect; wait for the host to return. */
			usleep(20000);
			continue;
		}
		if (dbcnet_decode(&dec, io->data, n, write_frame, NULL) < 0)
			fprintf(stderr, "dbc-impostor: decode: %s\n", strerror(errno));
	}
	return NULL;
}

/* Target -> host: TUN -> batched frames -> bulk IN. */
static void *in_thread(void *arg)
{
	(void)arg;
	static struct dbcnet_batch batch;
	struct usb_raw_ep_io *io = malloc(sizeof(*io) + sizeof(batch.buf));
	for (;;) {
		ssize_t n = dbcnet_fill_batch(tun, &batch, -1);
		if (n < 0)
			die("tun read");
		if (n == 0)
			continue;
		io->ep = ep_in;
		/* The host reads in chunks larger than one packet, so a transfer
		 * that ends on a packet boundary needs a ZLP to complete it. */
		io->flags = opt_zlp ? USB_RAW_IO_FLAGS_ZERO : 0;
		io->length = n;
		memcpy(io->data, batch.buf, n);
		if (ioctl(fd, USB_RAW_IOCTL_EP_WRITE, io) < 0)
			fprintf(stderr, "dbc-impostor: bulk in: %s\n", strerror(errno));
	}
	return NULL;
}

static void set_configuration(uint16_t value)
{
	if (value == 1 && ep_out < 0) {
		ep_out = ep_enable(EP_OUT_ADDR);
		ep_in = ep_enable(EP_IN_ADDR);
	}
	if (ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, 0) < 0)
		die("vbus draw");
	if (ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0) < 0)
		die("configure");
	ep0_read(0);
	if (value == 1 && !atomic_exchange(&data_started, true)) {
		pthread_t t;
		pthread_create(&t, NULL, out_thread, NULL);
		pthread_create(&t, NULL, in_thread, NULL);
		fprintf(stderr, "dbc-impostor: configured, bridging to %s\n",
			opt_tun);
	}
}

static void handle_standard(const struct usb_ctrlrequest *c)
{
	uint8_t buf[EP0_MAX];
	uint16_t wlength = le16toh(c->wLength);
	uint16_t value = le16toh(c->wValue);

	switch (c->bRequest) {
	case USB_REQ_GET_DESCRIPTOR: {
		size_t n = 0;
		switch (value >> 8) {
		case USB_DT_DEVICE: {
			struct usb_device_descriptor d = device_desc();
			memcpy(buf, &d, sizeof(d));
			n = sizeof(d);
			break;
		}
		case USB_DT_CONFIG:
			n = config_desc(buf);
			break;
		case USB_DT_BOS:
			n = opt_high_speed ? 0 : bos_desc(buf);
			break;
		case USB_DT_STRING:
			n = string_desc(value & 0xff, buf);
			break;
		}
		if (n)
			ep0_write(buf, n, wlength);
		else
			ep0_stall();
		return;
	}
	case USB_REQ_SET_CONFIGURATION:
		set_configuration(value);
		return;
	case USB_REQ_GET_CONFIGURATION:
		buf[0] = data_started ? 1 : 0;
		ep0_write(buf, 1, wlength);
		return;
	case USB_REQ_GET_INTERFACE:
		buf[0] = 0;
		ep0_write(buf, 1, wlength);
		return;
	case USB_REQ_GET_STATUS:
		buf[0] = (c->bRequestType & USB_RECIP_MASK) == USB_RECIP_DEVICE;
		buf[1] = 0;
		ep0_write(buf, 2, wlength);
		return;
	case USB_REQ_SET_INTERFACE:
	case USB_REQ_SET_FEATURE:
	case USB_REQ_CLEAR_FEATURE:
	case USB_REQ_SET_SEL:
	case USB_REQ_SET_ISOCH_DELAY:
		ep0_read(wlength);
		return;
	}
	ep0_stall();
}

static void ep0_loop(void)
{
	struct usb_raw_event *ev =
		malloc(sizeof(*ev) + sizeof(struct usb_ctrlrequest));
	for (;;) {
		ev->type = 0;
		ev->length = sizeof(struct usb_ctrlrequest);
		if (ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, ev) < 0)
			die("event fetch");
		switch (ev->type) {
		case USB_RAW_EVENT_CONNECT:
			fprintf(stderr, "dbc-impostor: connected\n");
			break;
		case USB_RAW_EVENT_RESET:
			fprintf(stderr, "dbc-impostor: bus reset\n");
			break;
		case USB_RAW_EVENT_DISCONNECT:
			fprintf(stderr, "dbc-impostor: disconnected\n");
			break;
		case USB_RAW_EVENT_CONTROL: {
			struct usb_ctrlrequest c;
			memcpy(&c, ev->data, sizeof(c));
			if ((c.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD)
				handle_standard(&c);
			else
				ep0_stall();
			break;
		}
		}
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dbc-impostor [--tun NAME] [--vid HEX] [--pid HEX]\n"
		"                    [--protocol N] [--udc-driver NAME]\n"
		"                    [--udc-device NAME] [--no-zlp]\n"
		"                    [--speed high|super]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "tun", required_argument, NULL, 't' },
		{ "vid", required_argument, NULL, 'v' },
		{ "pid", required_argument, NULL, 'p' },
		{ "protocol", required_argument, NULL, 'P' },
		{ "udc-driver", required_argument, NULL, 'D' },
		{ "udc-device", required_argument, NULL, 'd' },
		{ "no-zlp", no_argument, NULL, 'z' },
		{ "speed", required_argument, NULL, 's' },
		{ 0 },
	};
	int o;
	while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (o) {
		case 't': opt_tun = optarg; break;
		case 'v': opt_vid = strtoul(optarg, NULL, 16); break;
		case 'p': opt_pid = strtoul(optarg, NULL, 16); break;
		case 'P': opt_protocol = strtoul(optarg, NULL, 0); break;
		case 'D': opt_udc_driver = optarg; break;
		case 'd': opt_udc_device = optarg; break;
		case 'z': opt_zlp = false; break;
		case 's':
			if (!strcmp(optarg, "high"))
				opt_high_speed = true;
			else if (strcmp(optarg, "super"))
				usage();
			break;
		default: usage();
		}
	}

	tun = dbcnet_tun_open(opt_tun);
	if (tun < 0)
		die("open tun %s", opt_tun);

	fd = open("/dev/raw-gadget", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open /dev/raw-gadget");
	struct usb_raw_init init = {
		.speed = opt_high_speed ? USB_SPEED_HIGH : USB_SPEED_SUPER,
	};
	strncpy((char *)init.driver_name, opt_udc_driver, UDC_NAME_LENGTH_MAX - 1);
	strncpy((char *)init.device_name, opt_udc_device, UDC_NAME_LENGTH_MAX - 1);
	if (ioctl(fd, USB_RAW_IOCTL_INIT, &init) < 0)
		die("raw gadget init");
	if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0)
		die("raw gadget run");
	fprintf(stderr, "dbc-impostor: %04x:%04x on %s, waiting for host\n",
		opt_vid, opt_pid, opt_udc_device);
	ep0_loop();
}
