// SPDX-License-Identifier: GPL-2.0
/*
 * dbcnet - the debug host's end of a network link over the xHCI Debug
 * Capability, as a usbnet minidriver.
 *
 * The target is Linux's dbc_net, which presents the DbC as 1d6b:0012 where
 * ttyDBC is 1d6b:0010 and usb_debug's. 0012 is provisional until assigned.
 *
 * The framing is dbc_net's: a 4-byte header per IP packet (magic 0xdb5e,
 * then the payload length, both big-endian), the frames forming a byte
 * stream in each direction. A frame may span URBs, so the decoder keeps its
 * state between them. Packets go out batched, and every transfer ends on a
 * short packet or a ZLP, which the target's large receive TRBs need to
 * complete.
 */
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/if_arp.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/usbnet.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

/*
 * usbnet keeps byte queue limits from 6.19 on (commit 7ff14c52049e).
 * Before that, account for the transfers here, as usbnet would.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
#define DBCNET_OWN_BQL
#endif

/*
 * Newer usbnet clamps max_mtu to the hard MTU unless the driver sets this
 * flag. Kernels without the flag (up to 6.18 mainline, and the 5.10 to 6.6
 * LTS branches) have no clamp, so the empty value is right for them.
 */
#ifndef FLAG_NOMAXMTU
#define FLAG_NOMAXMTU		0
#endif

#define DBCNET_VID		0x1d6b
#define DBCNET_PID		0x0012
/* The DbC interface's class, Diagnostic Device, as the spec fixes it. */
#define DBCNET_CLASS		0xdc
#define DBCNET_MAGIC		0xdb5e
#define DBCNET_HDR		4
#define DBCNET_MAX_PAYLOAD	0xffff
/* A stream needs no particular URB size; this keeps several in flight. */
#define DBCNET_RX_URB_SIZE	SZ_16K
/* Frames are collected into one transfer up to this size. */
#define DBCNET_TX_BATCH		SZ_32K
/* How long a batch waits, behind transfers in flight, for more frames. */
#define DBCNET_TX_HOLD_NS	(100 * NSEC_PER_USEC)

struct dbcnet {
	struct usbnet		*dev;

	/* Stream decoder state: */
	u8			hdr[DBCNET_HDR];
	unsigned int		hdr_len;
	unsigned int		need;		/* payload bytes still to come */
	struct sk_buff		*skb;		/* NULL while discarding */

	/* The transfer being collected, under the netdev TX lock: */
	struct sk_buff		*tx_agg;
	unsigned int		tx_frames;
	struct hrtimer		tx_timer;	/* sends a held batch */
#ifdef DBCNET_OWN_BQL
	spinlock_t		bql_lock;	/* serializes completions */
#endif
};

static void dbcnet_rx_reset(struct dbcnet *rx)
{
	dev_kfree_skb_any(rx->skb);
	rx->skb = NULL;
	rx->hdr_len = 0;
	rx->need = 0;
}

static void dbcnet_tx_reset(struct dbcnet *d)
{
	dev_kfree_skb_any(d->tx_agg);
	d->tx_agg = NULL;
	d->tx_frames = 0;
}

static void dbcnet_deliver(struct usbnet *dev, struct sk_buff *skb)
{
	switch (skb->data[0] >> 4) {
	case 4:
		skb->protocol = htons(ETH_P_IP);
		break;
	case 6:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		dev->net->stats.rx_errors++;
		dev_kfree_skb_any(skb);
		return;
	}
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	usbnet_skb_return(dev, skb);
}

static int dbcnet_rx_fixup(struct usbnet *dev, struct sk_buff *in)
{
	struct dbcnet	*rx = dev->driver_priv;
	const u8		*p = in->data;
	unsigned int		len = in->len;
	unsigned int		n;

	while (len) {
		if (rx->need) {
			n = min(len, rx->need);
			if (rx->skb)
				skb_put_data(rx->skb, p, n);
			p += n;
			len -= n;
			rx->need -= n;
			if (!rx->need && rx->skb) {
				dbcnet_deliver(dev, rx->skb);
				rx->skb = NULL;
			}
			continue;
		}

		rx->hdr[rx->hdr_len++] = *p++;
		len--;
		if (rx->hdr_len < DBCNET_HDR)
			continue;

		if (get_unaligned_be16(rx->hdr) != DBCNET_MAGIC) {
			/* Out of sync: slide by one byte and look again. */
			memmove(rx->hdr, rx->hdr + 1, DBCNET_HDR - 1);
			rx->hdr_len--;
			dev->net->stats.rx_frame_errors++;
			continue;
		}

		rx->hdr_len = 0;
		rx->need = get_unaligned_be16(rx->hdr + 2);
		/* An empty frame is padding. */
		if (!rx->need)
			continue;
		rx->skb = netdev_alloc_skb(dev->net, rx->need);
		if (!rx->skb)
			dev->net->stats.rx_dropped++;
	}

	/* Frames went up one by one; usbnet frees the URB's skb. */
	return 1;
}

#ifdef DBCNET_OWN_BQL
static void dbcnet_tx_done(struct sk_buff *skb)
{
	struct usbnet		*dev = netdev_priv(skb->dev);
	struct dbcnet		*d = dev->driver_priv;
	unsigned long		flags;

	spin_lock_irqsave(&d->bql_lock, flags);
	netdev_completed_queue(skb->dev, 1, skb->len);
	spin_unlock_irqrestore(&d->bql_lock, flags);
}

static void dbcnet_bql_sent(struct usbnet *dev, struct sk_buff *agg)
{
	agg->dev = dev->net;
	agg->destructor = dbcnet_tx_done;
	netdev_sent_queue(dev->net, agg->len);
}
#else
static void dbcnet_bql_sent(struct usbnet *dev, struct sk_buff *agg)
{
}
#endif

/*
 * One transfer per packet would cost the target a TRB completion each, so
 * packets are collected and go out together. A batch goes when it is full,
 * or when the stack has no more for now and the link is idle. While
 * transfers are in flight, the link could not take it sooner anyway: it
 * waits for more, up to DBCNET_TX_HOLD_NS. That matters when a queue limit
 * (byte queue limits, usbnet's URB count) stops the queue mid-burst: the
 * qdisc hands the rest back one packet at a time, each without xmit_more.
 *
 * FLAG_MULTI_PACKET makes usbnet accept NULL as "collected". The timer
 * passes a NULL skb to send what is held.
 */
static struct sk_buff *dbcnet_tx_fixup(struct usbnet *dev, struct sk_buff *skb,
				       gfp_t flags)
{
	struct dbcnet		*d = dev->driver_priv;
	struct sk_buff		*agg = d->tx_agg;
	unsigned int		len;
	u8			*p;

	if (!skb)
		goto send;
	len = skb->len;

	if (!agg) {
		/* Room for a full batch and one more frame. */
		agg = alloc_skb(DBCNET_TX_BATCH + DBCNET_HDR +
				max(len, READ_ONCE(dev->net->mtu)), flags);
		if (!agg)
			goto drop;
		d->tx_agg = agg;
		d->tx_frames = 0;
	}

	/* Only a packet over an MTU raised mid-batch can miss. */
	if (len > DBCNET_MAX_PAYLOAD || skb_tailroom(agg) < DBCNET_HDR + len)
		goto drop;

	p = skb_put(agg, DBCNET_HDR);
	put_unaligned_be16(DBCNET_MAGIC, p);
	put_unaligned_be16(len, p + 2);
	skb_copy_bits(skb, 0, skb_put(agg, len), len);
	d->tx_frames++;
	dev_consume_skb_any(skb);
	goto flush;

drop:
	dev->net->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
flush:
	if (!d->tx_agg)
		return NULL;
	if (d->tx_agg->len < DBCNET_TX_BATCH &&
	    (netdev_xmit_more() || skb_queue_len_lockless(&dev->txq))) {
		if (!hrtimer_is_queued(&d->tx_timer))
			hrtimer_start(&d->tx_timer, DBCNET_TX_HOLD_NS,
				      HRTIMER_MODE_REL_SOFT);
		return NULL;
	}
send:
	agg = d->tx_agg;
	d->tx_agg = NULL;
	if (!agg)
		return NULL;
	if (!d->tx_frames) {
		dev_kfree_skb_any(agg);
		return NULL;
	}
	/* Count the IP packets, not our headers. */
	usbnet_set_skb_tx_stats(agg, d->tx_frames, -(long)(d->tx_frames * DBCNET_HDR));
	dbcnet_bql_sent(dev, agg);

	return agg;
}

static enum hrtimer_restart dbcnet_tx_timer(struct hrtimer *t)
{
	struct dbcnet		*d = container_of(t, struct dbcnet, tx_timer);
	struct net_device	*net = d->dev->net;

	netif_tx_lock(net);
	if (d->tx_agg) {
		/*
		 * A stopped queue would drop it: wait for the queue. A transmit
		 * that ran while this waited for the lock may have rearmed us.
		 */
		if (!netif_xmit_stopped(netdev_get_tx_queue(net, 0)))
			usbnet_start_xmit(NULL, net);
		else if (!hrtimer_is_queued(t))
			hrtimer_start(t, DBCNET_TX_HOLD_NS, HRTIMER_MODE_REL_SOFT);
	}
	netif_tx_unlock(net);

	return HRTIMER_NORESTART;
}

/* usbnet_open() calls this: URBs killed by a stop may have split a frame. */
static int dbcnet_reset(struct usbnet *dev)
{
	dbcnet_rx_reset(dev->driver_priv);
	dbcnet_tx_reset(dev->driver_priv);
#ifdef DBCNET_OWN_BQL
	/* The stop before waited for every transfer. */
	netdev_reset_queue(dev->net);
#endif
	return 0;
}

/* usbnet_stop() calls this with the queue stopped, before killing URBs. */
static int dbcnet_stop(struct usbnet *dev)
{
	struct dbcnet		*d = dev->driver_priv;

	hrtimer_cancel(&d->tx_timer);
	dbcnet_tx_reset(d);
	return 0;
}

/*
 * usbnet refuses an MTU that is a multiple of the packet size, so that a
 * full-size frame needs no ZLP. Here every frame carries a header, and every
 * transfer ends with a ZLP or a short packet anyway.
 */
static int dbcnet_change_mtu(struct net_device *net, int new_mtu)
{
	struct usbnet		*dev = netdev_priv(net);

	WRITE_ONCE(net->mtu, new_mtu);
	dev->hard_mtu = new_mtu + DBCNET_HDR;
	usbnet_update_max_qlen(dev);

	return 0;
}

/* usbnet's, less the Ethernet address handling and its MTU rule. */
static const struct net_device_ops dbcnet_netdev_ops = {
	.ndo_open		= usbnet_open,
	.ndo_stop		= usbnet_stop,
	.ndo_start_xmit		= usbnet_start_xmit,
	.ndo_tx_timeout		= usbnet_tx_timeout,
	.ndo_set_rx_mode	= usbnet_set_rx_mode,
	.ndo_change_mtu		= dbcnet_change_mtu,
};

static int dbcnet_bind(struct usbnet *dev, struct usb_interface *intf)
{
	struct net_device	*net = dev->net;
	struct dbcnet		*d;
	int			ret;

	ret = usbnet_get_endpoints(dev, intf);
	if (ret)
		return ret;

	d = kzalloc(sizeof(struct dbcnet), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&d->tx_timer, dbcnet_tx_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
#else
	hrtimer_init(&d->tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	d->tx_timer.function = dbcnet_tx_timer;
#endif
#ifdef DBCNET_OWN_BQL
	spin_lock_init(&d->bql_lock);
#endif
	dev->driver_priv = d;

	/* Raw IP on a point-to-point link: no link-layer header at all. */
	net->netdev_ops = &dbcnet_netdev_ops;
	net->header_ops = NULL;
	net->type = ARPHRD_NONE;
	net->hard_header_len = 0;
	net->addr_len = 0;
	net->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	net->max_mtu = DBCNET_MAX_PAYLOAD;
	strscpy(net->name, "dbc%d", sizeof(net->name));
	set_bit(EVENT_NO_IP_ALIGN, &dev->flags);

	dev->hard_mtu = net->mtu + DBCNET_HDR;
	dev->rx_urb_size = DBCNET_RX_URB_SIZE;

	return 0;
}

static void dbcnet_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct dbcnet	*rx = dev->driver_priv;

	if (rx) {
		hrtimer_cancel(&rx->tx_timer);
		dbcnet_rx_reset(rx);
		dbcnet_tx_reset(rx);
		kfree(rx);
		dev->driver_priv = NULL;
	}
}

static const struct driver_info dbcnet_info = {
	.description	= "Network over xHCI DbC",
	.flags		= FLAG_POINTTOPOINT | FLAG_NOARP | FLAG_SEND_ZLP |
			  FLAG_MULTI_PACKET | FLAG_RX_ASSEMBLE | FLAG_NOMAXMTU,
	.bind		= dbcnet_bind,
	.unbind		= dbcnet_unbind,
	.reset		= dbcnet_reset,
	.stop		= dbcnet_stop,
	.rx_fixup	= dbcnet_rx_fixup,
	.tx_fixup	= dbcnet_tx_fixup,
};

static const struct usb_device_id dbcnet_ids[] = {
	{
		USB_DEVICE_INTERFACE_CLASS(DBCNET_VID, DBCNET_PID, DBCNET_CLASS),
		.driver_info = (unsigned long)&dbcnet_info,
	},
	{ },
};
MODULE_DEVICE_TABLE(usb, dbcnet_ids);

static struct usb_driver dbcnet_driver = {
	.name			= "dbcnet",
	.id_table		= dbcnet_ids,
	.probe			= usbnet_probe,
	.disconnect		= usbnet_disconnect,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(dbcnet_driver);

MODULE_DESCRIPTION("Network link to a Linux target over the xHCI Debug Capability");
MODULE_LICENSE("GPL");
