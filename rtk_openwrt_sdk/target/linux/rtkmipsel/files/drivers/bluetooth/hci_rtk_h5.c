/*
 *
 *  Bluetooth HCI UART driver
 *
 *  Copyright (C) 2011-2014  wifi_fae<wifi_fae@realtek.com.tw>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 *
 *     Date      Vesion       Author                Comment
 *  ----------  --------   -------------       -------------------------------------
 *  2013-06-06   1.0.0      gordon_yang         basic version, optmize check for hang
 *
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>

#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/signal.h>
#include <linux/ioctl.h>
#include <linux/skbuff.h>
#include <linux/bitrev.h>
#include <asm/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

//for check sleep
//#include <linux/wakelock.h>
#include <linux/serial_core.h>

#include "hci_uart.h"

#define VERSION "1.0.1"

//#define RTK_BLUE_SLEEP
static int txcrc = 1;

static int need_check_bt_state = 0; // 1 :for Android +8723AS; 0:for Linux


//static int hciextn = 1;
#define H5_BTSTATE_CHECK_CONFIG_PATH      "/system/etc/firmware/rtl8723as/need_check_bt_state"

#define H5_TXWINSIZE	4
#define H5_ACK_PKT	0x00
#define H5_LE_PKT	    0x0F
#define H5_VDRSPEC_PKT	0x0E

#define TX_TIMER_INTERVAL      2
#define STACK_TXDATA       0x01
#define STACK_SLEEP		   0x02

#ifdef RTK_BLUE_SLEEP
//sleep check begin
struct h5_sleep_info{
	atomic_t wake_sleep; //1: can sleep, value 0 need wake up
	struct uart_port *uport;
	struct wake_lock wake_lock;
};

/** Global state flags */
static unsigned long flags;

static struct h5_sleep_info *hsi;
#endif

static struct hci_dev *h5_hci_hdev;
/** Lock for state transitions */
static spinlock_t rw_lock;

#ifdef RTK_BLUE_SLEEP
static void h5_sleep_tx_timer_expire(unsigned long data);
static DEFINE_TIMER(h5_tx_timer, h5_sleep_tx_timer_expire, 0, 0);
static int h5_parse_hci_event(struct notifier_block *this, unsigned long event, void *data);
/** Notifier block for HCI events */
struct notifier_block hci_event_nblock = {
	.notifier_call = h5_parse_hci_event,
};

static void h5_sleep_work(struct work_struct *work);
DECLARE_DELAYED_WORK(sleep_workqueue, h5_sleep_work);
//sleep check end
#endif

struct h5_struct {
	struct sk_buff_head unack;	/* Unack'ed packets queue */
	struct sk_buff_head rel;	/* Reliable packets queue */
	struct sk_buff_head unrel;	/* Unreliable packets queue */

	unsigned long rx_count;
	struct	sk_buff *rx_skb;
	u8	rxseq_txack;		/* rxseq == txack. */
	u8	rxack;			/* Last packet sent by us that the peer ack'ed */
	struct	timer_list th5;

	u8 is_checking;

	enum {
		H5_W4_PKT_DELIMITER,
		H5_W4_PKT_START,
		H5_W4_HDR,
		H5_W4_DATA,
		H5_W4_CRC
	} rx_state;

	enum {
		H5_ESCSTATE_NOESC,
		H5_ESCSTATE_ESC
	} rx_esc_state;

	u8	use_crc;
	u16	message_crc;
	u8	txack_req;		/* Do we need to send ack's to the peer? */

	/* Reliable packet sequence number - used to assign seq to each rel pkt. */
	u8	msgq_txseq;
};


static void h5_bt_state_err_worker(struct work_struct *private_);
static void h5_bt_state_check_worker(struct work_struct *private_);
static DECLARE_DELAYED_WORK(bt_state_err_work, h5_bt_state_err_worker);
static DECLARE_DELAYED_WORK(bt_state_check_work, h5_bt_state_check_worker);
static struct hci_uart *hci_uart_info = NULL;

static struct mutex sem_exit;

/* ---- H5 CRC calculation ---- */

/* Table for calculating CRC for polynomial 0x1021, LSB processed first,
initial value 0xffff, bits shifted in reverse order. */

static const u16 crc_table[] = {
	0x0000, 0x1081, 0x2102, 0x3183,
	0x4204, 0x5285, 0x6306, 0x7387,
	0x8408, 0x9489, 0xa50a, 0xb58b,
	0xc60c, 0xd68d, 0xe70e, 0xf78f
};

/* Initialise the crc calculator */
#define H5_CRC_INIT(x) x = 0xffff

/*
   Update crc with next data byte

   Implementation note
        The data byte is treated as two nibbles.  The crc is generated
        in reverse, i.e., bits are fed into the register from the top.
*/
static void h5_crc_update(u16 *crc, u8 d)
{
	u16 reg = *crc;

	reg = (reg >> 4) ^ crc_table[(reg ^ d) & 0x000f];
	reg = (reg >> 4) ^ crc_table[(reg ^ (d >> 4)) & 0x000f];

	*crc = reg;
}

/* ---- H5 core ---- */

static void h5_slip_msgdelim(struct sk_buff *skb)
{
	const char pkt_delim = 0xc0;

	memcpy(skb_put(skb, 1), &pkt_delim, 1);
}

static void h5_slip_one_byte(struct sk_buff *skb, u8 c)
{
	const char esc_c0[2] = { 0xdb, 0xdc };
	const char esc_db[2] = { 0xdb, 0xdd };
	const char esc_11[2] = { 0xdb, 0xde };
	const char esc_13[2] = { 0xdb, 0xdf };

	switch (c) {
	case 0xc0:
		memcpy(skb_put(skb, 2), &esc_c0, 2);
		break;
	case 0xdb:
		memcpy(skb_put(skb, 2), &esc_db, 2);
		break;
	case 0x11:
		memcpy(skb_put(skb, 2), &esc_11, 2);
		break;
	case 0x13:
		memcpy(skb_put(skb, 2), &esc_13, 2);
		break;
	default:
		memcpy(skb_put(skb, 1), &c, 1);
	}
}

static int h5_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct h5_struct *h5 = hu->priv;

	if (skb->len > 0xFFF) { //Pkt length must be less than 4095 bytes
		BT_ERR("Packet too long");
		kfree_skb(skb);
		return 0;
	}

	switch (bt_cb(skb)->pkt_type) {
	case HCI_ACLDATA_PKT:
	case HCI_COMMAND_PKT:
	    skb_queue_tail(&h5->rel, skb);
	    break;

	case HCI_SCODATA_PKT:
	    skb_queue_tail(&h5->unrel, skb);
	    break;
	case H5_LE_PKT:
	case H5_ACK_PKT:
	case H5_VDRSPEC_PKT:
	    skb_queue_tail(&h5->unrel, skb);	/* 3-wire LinkEstablishment*/
	    break;

	default:
	    BT_ERR("Unknown packet type");
	    kfree_skb(skb);
	    break;
	}

	return 0;
}

static struct sk_buff *h5_prepare_pkt(struct h5_struct *h5, u8 *data,
		int len, int pkt_type)
{
	struct sk_buff *nskb;
	u8 hdr[4], chan;
	u16 H5_CRC_INIT(h5_txmsg_crc);
	int rel, i;

	switch (pkt_type) {
	case HCI_ACLDATA_PKT:
	    chan = 2;	/* 3-wire ACL channel */
	    rel = 1;	/* reliable channel */
	    break;
	case HCI_COMMAND_PKT:
	    chan = 1;	/* 3-wire cmd channel */
	    rel = 1;	/* reliable channel */
	    break;
	case HCI_EVENT_PKT:
	    chan = 4;	/* 3-wire cmd channel */
	    rel = 1;	/* reliable channel */
	    break;
	case HCI_SCODATA_PKT:
	    chan = 3;	/* 3-wire SCO channel */
	    rel = 0;	/* unreliable channel */
            break;
	case H5_LE_PKT:
	    chan = 15;	/* 3-wire LinkEstablishment channel */
	    rel = 0;	/* unreliable channel */
	    break;
	case H5_ACK_PKT:
	    chan = 0;	/* 3-wire ACK channel */
	    rel = 0;	/* unreliable channel */
	    break;
	case H5_VDRSPEC_PKT:
	    chan = 14;	/* 3-wire Vendor Specific channel */
	    rel = 0;	/* unreliable channel */
	    break;
	default:
	    BT_ERR("Unknown packet type");
	    return NULL;
	}


	/* Max len of packet: (original len +4(h5 hdr) +2(crc))*2
	   (because bytes 0xc0 and 0xdb are escaped, worst case is
	   when the packet is all made of 0xc0 and 0xdb :) )
	   + 2 (0xc0 delimiters at start and end). */

	nskb = alloc_skb((len + 6) * 2 + 2, GFP_ATOMIC);
	if (!nskb)
		return NULL;

	bt_cb(nskb)->pkt_type = pkt_type;

	h5_slip_msgdelim(nskb);

	hdr[0] = h5->rxseq_txack << 3;
	h5->txack_req = 0;
	BT_DBG("We request packet no %u to card", h5->rxseq_txack);

	if (rel) {
		hdr[0] |= 0x80 + h5->msgq_txseq;
		BT_DBG("Sending packet with seqno %u", h5->msgq_txseq);
		h5->msgq_txseq = (h5->msgq_txseq + 1) & 0x07;
	}

	if (h5->use_crc)
		hdr[0] |= 0x40;

	hdr[1] = ((len << 4) & 0xff) | chan;
	hdr[2] = len >> 4;
	hdr[3] = ~(hdr[0] + hdr[1] + hdr[2]);

	/* Put H5 header */
	for (i = 0; i < 4; i++) {
		h5_slip_one_byte(nskb, hdr[i]);

		if (h5->use_crc)
			h5_crc_update(&h5_txmsg_crc, hdr[i]);
	}

	/* Put payload */
	for (i = 0; i < len; i++) {
		h5_slip_one_byte(nskb, data[i]);

		if (h5->use_crc)
			h5_crc_update(&h5_txmsg_crc, data[i]);
	}

	/* Put CRC */
	if (h5->use_crc) {
		h5_txmsg_crc = bitrev16(h5_txmsg_crc);
		h5_slip_one_byte(nskb, (u8) ((h5_txmsg_crc >> 8) & 0x00ff));
		h5_slip_one_byte(nskb, (u8) (h5_txmsg_crc & 0x00ff));
	}

	h5_slip_msgdelim(nskb);
	return nskb;
}

/* This is a rewrite of pkt_avail in AH5 */
static struct sk_buff *h5_dequeue(struct hci_uart *hu)
{
	struct h5_struct *h5 = hu->priv;
	unsigned long flags;
	struct sk_buff *skb;

	/* First of all, check for unreliable messages in the queue,
	   since they have priority */
#if 0
	if (need_check_bt_state) {
		schedule_delayed_work(&bt_state_check_work, HZ * 60);
	}
#endif
	if ((skb = skb_dequeue(&h5->unrel)) != NULL) {
		struct sk_buff *nskb = h5_prepare_pkt(h5, skb->data, skb->len, bt_cb(skb)->pkt_type);
		if (nskb) {
			kfree_skb(skb);
			return nskb;
		} else {
			skb_queue_head(&h5->unrel, skb);
			BT_ERR("Could not dequeue pkt because alloc_skb failed");
		}
	}

	/* Now, try to send a reliable pkt. We can only send a
	   reliable packet if the number of packets sent but not yet ack'ed
	   is < than the winsize */

	spin_lock_irqsave_nested(&h5->unack.lock, flags, SINGLE_DEPTH_NESTING);

	if (h5->unack.qlen < H5_TXWINSIZE && (skb = skb_dequeue(&h5->rel)) != NULL) {
		struct sk_buff *nskb = h5_prepare_pkt(h5, skb->data, skb->len, bt_cb(skb)->pkt_type);
		if (nskb) {
			__skb_queue_tail(&h5->unack, skb);
			mod_timer(&h5->th5, jiffies + HZ / 4);
			spin_unlock_irqrestore(&h5->unack.lock, flags);
			return nskb;
		} else {
			skb_queue_head(&h5->rel, skb);
			BT_ERR("Could not dequeue pkt because alloc_skb failed");
		}
	}

	spin_unlock_irqrestore(&h5->unack.lock, flags);

	/* We could not send a reliable packet, either because there are
	   none or because there are too many unack'ed pkts. Did we receive
	   any packets we have not acknowledged yet ? */

	if (h5->txack_req) {
		/* if so, craft an empty ACK pkt and send it on H5 unreliable
		   channel 0 */
		struct sk_buff *nskb = h5_prepare_pkt(h5, NULL, 0, H5_ACK_PKT);
		return nskb;
	}
/*
	if (need_check_bt_state) {
		schedule_delayed_work(&bt_state_check_work, HZ * 15);
	}
*/
	/* We have nothing to send */
	return NULL;
}

static int h5_flush(struct hci_uart *hu)
{
	BT_DBG("hu %p", hu);
	return 0;
}

/* Remove ack'ed packets */
static void h5_pkt_cull(struct h5_struct *h5)
{
	struct sk_buff *skb, *tmp;
	unsigned long flags;
	int i, pkts_to_be_removed;
	u8 seqno;

	spin_lock_irqsave(&h5->unack.lock, flags);

	pkts_to_be_removed = skb_queue_len(&h5->unack);
	seqno = h5->msgq_txseq;

	while (pkts_to_be_removed) {
		if (h5->rxack == seqno)
			break;
		pkts_to_be_removed--;
		seqno = (seqno - 1) & 0x07;
	}

	if (h5->rxack != seqno)
		BT_ERR("Peer acked invalid packet");

	BT_DBG("Removing %u pkts out of %u, up to seqno %u",
	       pkts_to_be_removed, skb_queue_len(&h5->unack),
	       (seqno - 1) & 0x07);

	i = 0;
	skb_queue_walk_safe(&h5->unack, skb, tmp) {
		if (i >= pkts_to_be_removed)
			break;
		i++;

		__skb_unlink(skb, &h5->unack);
		kfree_skb(skb);
	}

	if (skb_queue_empty(&h5->unack))
		del_timer(&h5->th5);

	spin_unlock_irqrestore(&h5->unack.lock, flags);

	if (i != pkts_to_be_removed)
		BT_ERR("Removed only %u out of %u pkts", i, pkts_to_be_removed);
}

/* Handle H5 link-establishment packets. When we
   detect a "sync" packet, symptom that the BT module has reset,
   we do nothing :) (yet) */
static void h5_handle_le_pkt(struct hci_uart *hu)
{
	struct h5_struct *h5 = hu->priv;
	u8 conf_pkt[2]     = { 0x03, 0xfc};
	u8 conf_rsp_pkt[3] = { 0x04, 0x7b, 0x00};
	u8 sync_pkt[2]     = { 0x01, 0x7e};
	u8 sync_rsp_pkt[2] = { 0x02, 0x7d};

	u8 wakeup_pkt[2]   = { 0x05, 0xfa};
	u8 woken_pkt[2]    = { 0x06, 0xf9};
	u8 sleep_pkt[2]    = { 0x07, 0x78};

	/* spot "conf" pkts and reply with a "conf rsp" pkt */
	if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], conf_pkt, 2)) {
		struct sk_buff *nskb = alloc_skb(3, GFP_ATOMIC);

		BT_DBG("Found a LE conf pkt");
		if (!nskb)
			return;

		conf_rsp_pkt[2] |= txcrc << 0x4; //crc check enable, version no = 0. needed to be as avariable.
		memcpy(skb_put(nskb, 3), conf_rsp_pkt, 3);
		bt_cb(nskb)->pkt_type = H5_LE_PKT;

		skb_queue_head(&h5->unrel, nskb);
		hci_uart_tx_wakeup(hu);
	}
	/* spot "conf resp" pkts*/
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], conf_rsp_pkt, 2)) {
		BT_DBG("Found a LE conf resp pkt, device go into active state");
	        txcrc = (h5->rx_skb->data[6] >> 0x4) & 0x1;
	}

	/* Spot "sync" pkts. If we find one...disaster! */
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], sync_pkt, 2)) {
		BT_ERR("Found a LE sync pkt, card has reset");
		//DO Something here
	}
	/* Spot "sync resp" pkts. If we find one...disaster! */
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], sync_rsp_pkt, 2)) {
		BT_ERR("Found a LE sync resp pkt, device go into initialized state");
		//      DO Something here
	}
	/* Spot "wakeup" pkts. reply woken message when in active mode */
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], wakeup_pkt, 2)) {
		struct sk_buff *nskb = alloc_skb(2, GFP_ATOMIC);

		BT_ERR("Found a LE Wakeup pkt, and reply woken message");
		//      DO Something here

		memcpy(skb_put(nskb, 2), woken_pkt, 2);
		bt_cb(nskb)->pkt_type = H5_LE_PKT;

		skb_queue_head(&h5->unrel, nskb);
		hci_uart_tx_wakeup(hu);
	}
	/* Spot "woken" pkts. receive woken message from device */
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], woken_pkt, 2)) {
		BT_ERR("Found a LE woken pkt from device");
		//      DO Something here
	}
	/* Spot "Sleep" pkts*/
	else if (h5->rx_skb->data[1] >> 4 == 2 && h5->rx_skb->data[2] == 0 &&
			!memcmp(&h5->rx_skb->data[4], sleep_pkt, 2)) {
		BT_ERR("Found a LE Sleep pkt");
		//      DO Something here
	}

}

static inline void h5_unslip_one_byte(struct h5_struct *h5, unsigned char byte)
{
	const u8 c0   = 0xc0, db   = 0xdb;
	const u8 oof1 = 0x11, oof2 = 0x13;

	switch (h5->rx_esc_state) {
	case H5_ESCSTATE_NOESC:
		switch (byte) {
		case 0xdb:
			h5->rx_esc_state = H5_ESCSTATE_ESC;
			break;
		default:
			memcpy(skb_put(h5->rx_skb, 1), &byte, 1);
			if ((h5->rx_skb-> data[0] & 0x40) != 0 &&
					h5->rx_state != H5_W4_CRC)
				h5_crc_update(&h5->message_crc, byte);
			h5->rx_count--;
		}
		break;

	case H5_ESCSTATE_ESC:
		switch (byte) {
		case 0xdc:
			memcpy(skb_put(h5->rx_skb, 1), &c0, 1);
			if ((h5->rx_skb-> data[0] & 0x40) != 0 &&
					h5->rx_state != H5_W4_CRC)
				h5_crc_update(&h5-> message_crc, 0xc0);
			h5->rx_esc_state = H5_ESCSTATE_NOESC;
			h5->rx_count--;
			break;

		case 0xdd:
			memcpy(skb_put(h5->rx_skb, 1), &db, 1);
			if ((h5->rx_skb-> data[0] & 0x40) != 0 &&
					h5->rx_state != H5_W4_CRC)
				h5_crc_update(&h5-> message_crc, 0xdb);
			h5->rx_esc_state = H5_ESCSTATE_NOESC;
			h5->rx_count--;
			break;

		case 0xde:
			memcpy(skb_put(h5->rx_skb, 1), &oof1, 1);
			if ((h5->rx_skb-> data[0] & 0x40) != 0 && h5->rx_state != H5_W4_CRC)
				h5_crc_update(&h5-> message_crc, oof1);
			h5->rx_esc_state = H5_ESCSTATE_NOESC;
			h5->rx_count--;
			break;

		case 0xdf:
			memcpy(skb_put(h5->rx_skb, 1), &oof2, 1);
			if ((h5->rx_skb-> data[0] & 0x40) != 0 && h5->rx_state != H5_W4_CRC)
				h5_crc_update(&h5-> message_crc, oof2);
			h5->rx_esc_state = H5_ESCSTATE_NOESC;
			h5->rx_count--;
			break;

		default:
			BT_ERR ("Invalid byte %02x after esc byte", byte);
			kfree_skb(h5->rx_skb);
			h5->rx_skb = NULL;
			h5->rx_state = H5_W4_PKT_DELIMITER;
			h5->rx_count = 0;
		}
	}
}

static void h5_complete_rx_pkt(struct hci_uart *hu)
{
	struct h5_struct *h5 = hu->priv;
	int pass_up;

	if (h5->rx_skb->data[0] & 0x80) {	/* reliable pkt */
		BT_DBG("Received seqno %u from card", h5->rxseq_txack);
		h5->rxseq_txack++;
		h5->rxseq_txack %= 0x8;
		h5->txack_req    = 1;

		/* If needed, transmit an ack pkt */
		hci_uart_tx_wakeup(hu);
	}

	h5->rxack = (h5->rx_skb->data[0] >> 3) & 0x07;
	BT_DBG("Request for pkt %u from card", h5->rxack);

	h5_pkt_cull(h5);

	if ((h5->rx_skb->data[1] & 0x0f) == 2 &&
			h5->rx_skb->data[0] & 0x80) {
		bt_cb(h5->rx_skb)->pkt_type = HCI_ACLDATA_PKT;
		pass_up = 1;
	} else if ((h5->rx_skb->data[1] & 0x0f) == 4 &&
			h5->rx_skb->data[0] & 0x80) {
		bt_cb(h5->rx_skb)->pkt_type = HCI_EVENT_PKT;
		pass_up = 1;

	if (need_check_bt_state) {
		/*
		 * after having received packets from controller, del hungup timer
		*/
		if (h5->is_checking)
		{
			h5->is_checking = false;
			printk("cancle state err work\n");
			//cancel_delayed_work(&bt_state_check_work);
			cancel_delayed_work(&bt_state_err_work);
			schedule_delayed_work(&bt_state_check_work, HZ * 60);
			mutex_unlock(&sem_exit);
		}
	}

	} else if ((h5->rx_skb->data[1] & 0x0f) == 3) {
		bt_cb(h5->rx_skb)->pkt_type = HCI_SCODATA_PKT;
		pass_up = 1;
	} else if ((h5->rx_skb->data[1] & 0x0f) == 15 &&
			!(h5->rx_skb->data[0] & 0x80)) {
		//h5_handle_le_pkt(hu);//Link Establishment Pkt
		pass_up = 0;
	} else if ((h5->rx_skb->data[1] & 0x0f) == 1 &&
			h5->rx_skb->data[0] & 0x80) {
		bt_cb(h5->rx_skb)->pkt_type = HCI_COMMAND_PKT;
		pass_up = 1;
	} else if ((h5->rx_skb->data[1] & 0x0f) == 14) {
		bt_cb(h5->rx_skb)->pkt_type = H5_VDRSPEC_PKT;
		pass_up = 1;
	} else
		pass_up = 0;

	if (!pass_up) {
		//struct hci_event_hdr hdr;
		u8 desc = (h5->rx_skb->data[1] & 0x0f);

		if (desc != H5_ACK_PKT && desc != H5_LE_PKT) {
#if 0
			if (hciextn) {
				desc |= 0xc0;
				skb_pull(h5->rx_skb, 4);
				memcpy(skb_push(h5->rx_skb, 1), &desc, 1);

				hdr.evt = 0xff;
				hdr.plen = h5->rx_skb->len;
				memcpy(skb_push(h5->rx_skb, HCI_EVENT_HDR_SIZE), &hdr, HCI_EVENT_HDR_SIZE);
				bt_cb(h5->rx_skb)->pkt_type = HCI_EVENT_PKT;

				hci_recv_frame(h5->rx_skb);
			} else {
#endif
				BT_ERR ("Packet for unknown channel (%u %s)",
					h5->rx_skb->data[1] & 0x0f,
					h5->rx_skb->data[0] & 0x80 ?
					"reliable" : "unreliable");
				kfree_skb(h5->rx_skb);
//			}
		} else
			kfree_skb(h5->rx_skb);
	} else {
		/* Pull out H5 hdr */
		skb_pull(h5->rx_skb, 4);
#if 0
		if (need_check_bt_state) {
			schedule_delayed_work(&bt_state_check_work, HZ * 60);
		}
#endif
		hci_recv_frame(h5->rx_skb);
	}

	h5->rx_state = H5_W4_PKT_DELIMITER;
	h5->rx_skb = NULL;
}

static u16 bscp_get_crc(struct h5_struct *h5)
{
	return get_unaligned_be16(&h5->rx_skb->data[h5->rx_skb->len - 2]);
}

/* Recv data */
static int h5_recv(struct hci_uart *hu, void *data, int count)
{
	struct h5_struct *h5 = hu->priv;
	register unsigned char *ptr;

	BT_DBG("hu %p count %d rx_state %d rx_count %ld",
		hu, count, h5->rx_state, h5->rx_count);

	ptr = data;
	while (count) {
		if (h5->rx_count) {
			if (*ptr == 0xc0) {
				BT_ERR("Short H5 packet");
				kfree_skb(h5->rx_skb);
				h5->rx_state = H5_W4_PKT_START;
				h5->rx_count = 0;
			} else
				h5_unslip_one_byte(h5, *ptr);

			ptr++; count--;
			continue;
		}

		switch (h5->rx_state) {
		case H5_W4_HDR:
			if ((0xff & (u8) ~ (h5->rx_skb->data[0] + h5->rx_skb->data[1] +
					h5->rx_skb->data[2])) != h5->rx_skb->data[3]) {
				BT_ERR("Error in H5 hdr checksum");
				kfree_skb(h5->rx_skb);
				h5->rx_state = H5_W4_PKT_DELIMITER;
				h5->rx_count = 0;
				continue;
			}
			if (h5->rx_skb->data[0] & 0x80	/* reliable pkt */
			    		&& (h5->rx_skb->data[0] & 0x07) != h5->rxseq_txack) {
				BT_ERR ("Out-of-order packet arrived, got %u expected %u",
					h5->rx_skb->data[0] & 0x07, h5->rxseq_txack);

				h5->txack_req = 1;
				hci_uart_tx_wakeup(hu);
				kfree_skb(h5->rx_skb);
				h5->rx_state = H5_W4_PKT_DELIMITER;
				h5->rx_count = 0;
				continue;
			}
			h5->rx_state = H5_W4_DATA;
			h5->rx_count = (h5->rx_skb->data[1] >> 4) +
					(h5->rx_skb->data[2] << 4);	/* May be 0 */
			continue;

		case H5_W4_DATA:
			if (h5->rx_skb->data[0] & 0x40) {	/* pkt with crc */
				h5->rx_state = H5_W4_CRC;
				h5->rx_count = 2;
			} else
				h5_complete_rx_pkt(hu);
			continue;

		case H5_W4_CRC:
			if (bitrev16(h5->message_crc) != bscp_get_crc(h5)) {
				BT_ERR ("Checksum failed: computed %04x received %04x",
					bitrev16(h5->message_crc),
					bscp_get_crc(h5));

				kfree_skb(h5->rx_skb);
				h5->rx_state = H5_W4_PKT_DELIMITER;
				h5->rx_count = 0;
				continue;
			}
			skb_trim(h5->rx_skb, h5->rx_skb->len - 2);
			h5_complete_rx_pkt(hu);
			continue;

		case H5_W4_PKT_DELIMITER:
			switch (*ptr) {
			case 0xc0:
				h5->rx_state = H5_W4_PKT_START;
				break;
			default:
				/*BT_ERR("Ignoring byte %02x", *ptr);*/
				break;
			}
			ptr++; count--;
			break;

		case H5_W4_PKT_START:
			switch (*ptr) {
			case 0xc0:
				ptr++; count--;
				break;

			default:
				h5->rx_state = H5_W4_HDR;
				h5->rx_count = 4;
				h5->rx_esc_state = H5_ESCSTATE_NOESC;
				H5_CRC_INIT(h5->message_crc);

				/* Do not increment ptr or decrement count
				 * Allocate packet. Max len of a H5 pkt=
				 * 0xFFF (payload) +4 (header) +2 (crc) */

				//h5->rx_skb = bt_skb_alloc(0x1005, GFP_ATOMIC);
				//gordon change to 1024 for 256m mem
				h5->rx_skb = bt_skb_alloc(0x640, GFP_ATOMIC);
				if (!h5->rx_skb) {
					BT_ERR("Can't allocate mem for new packet size is 640");
					h5->rx_state = H5_W4_PKT_DELIMITER;
					h5->rx_count = 0;
					return 0;
				}
				h5->rx_skb->dev = (void *) hu->hdev;
				break;
			}
			break;
		}
	}
	return count;
}

	/* Arrange to retransmit all messages in the relq. */
static void h5_timed_event(unsigned long arg)
{
	struct hci_uart *hu = (struct hci_uart *) arg;
	struct h5_struct *h5 = hu->priv;
	struct sk_buff *skb;
	unsigned long flags;

	BT_DBG("hu %p retransmitting %u pkts", hu, h5->unack.qlen);

	spin_lock_irqsave_nested(&h5->unack.lock, flags, SINGLE_DEPTH_NESTING);

	while ((skb = __skb_dequeue_tail(&h5->unack)) != NULL) {
		h5->msgq_txseq = (h5->msgq_txseq - 1) & 0x07;
		skb_queue_head(&h5->rel, skb);
	}

	spin_unlock_irqrestore(&h5->unack.lock, flags);

	hci_uart_tx_wakeup(hu);
}

static void h5_send_uevent(struct hci_uart* hu)
{
	char BT_restart[] = "BTRESTART=1";
	char *env_p[] = {
		BT_restart,
		NULL
	};
	int ret = -1;

	if (hu && hu->hdev)
	{
		ret = kobject_uevent_env(&hu->hdev->dev.kobj, KOBJ_OFFLINE, env_p);
		//printk("send event to upper layer:%x", ret);
	}
	else
		BT_INFO("h5_send_uevent without pointer exists");
}


static void h5_bt_state_err_worker(struct work_struct *private_)
{
	printk("Realtek: BT is NOT working now, try notify\n");

	if (hci_uart_info){
		h5_send_uevent(hci_uart_info);
	}
	mutex_unlock(&sem_exit);
}

/*
 * timer function to check if controller state
*/
static void h5_bt_state_check_worker(struct work_struct *private_)
{
	struct hci_uart *hu = hci_uart_info;
	struct h5_struct *h5 = hu->priv;
	struct sk_buff* pollcmd = NULL;
	u8 cmd[3] = {0};
	int ret=0;

	printk("Realtek to check4hung\n");
	ret = mutex_lock_interruptible(&sem_exit);

	if (ret !=0)
	{
		printk("Realtek mutex lock interrupted:%x, %s()\n", ret, __func__);
	}
	//send command and wait for any response.
	cmd[0] = 0x22;
	cmd[1] = 0xfc;
	pollcmd = bt_skb_alloc(3, GFP_ATOMIC);
	if (!pollcmd)
	{
		BT_ERR("allocate buffer for poll cmd error");
		return;
	}

	skb_put(pollcmd, sizeof(cmd));
	bt_cb(pollcmd)->pkt_type = HCI_COMMAND_PKT;
	/*
	 *  It's judged that controller has hung up
	 *  if no response received within 3HZ
	*/
	h5->is_checking = 1;
	schedule_delayed_work(&bt_state_err_work, HZ * 5);
	/*
	 *  make sure the bt_state_err_work perform completely
	*/
	memcpy(pollcmd->data, cmd, sizeof(cmd));
	skb_queue_head(&h5->rel, pollcmd);
	hci_uart_tx_wakeup(hu);
}

static int h5_config_state_check(void) {
	char    *config_path=H5_BTSTATE_CHECK_CONFIG_PATH;
	char    *buffer=NULL;
	struct file   *filp=NULL;
	mm_segment_t old_fs = get_fs();

	int result=0;
	set_fs (KERNEL_DS);

	//open file
	filp = filp_open(config_path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
	    // BT_ERR("open h5 bt state check config file fail?, chmod?\n");
	     result=-1;
	   goto error2;
	  }

	if(!(filp->f_op) || !(filp->f_op->read) ) {
	     BT_ERR("file %s cann't readable?\n",config_path);
	     result = -1;
	     goto error1;
	}

	buffer = kmalloc(1, GFP_KERNEL);
	if(buffer==NULL) {
		BT_ERR("alllocate mem for file fail?\n");
		result = -1;
		goto error1;
	}

	if(filp->f_op->read(filp, buffer, 1, &filp->f_pos)<0) {
		BT_ERR("read file error?\n");
		result = -1;
		goto error1;
	}

	if(memcmp(buffer,"0",1)==0) {
		//check bt state off
		need_check_bt_state = 0;
		BT_INFO("close check bt state");
	}
	if(memcmp(buffer,"1",1)==0) {
		//check bt state on
		need_check_bt_state = 1;
		BT_INFO("open check bt state");
	}

error1:
	if(buffer)
	 kfree(buffer);

	if(filp_close(filp,NULL))
	   BT_ERR("Config_FileOperation:close file fail\n");

error2:
	set_fs (old_fs);

	return result;
}

#ifdef RTK_BLUE_SLEEP
//for sleep check begin
static int h5_can_sleep(void)
{
	//printk("%s(), wake_sleep:%x, uport:%p\n", __func__, atomic_read(&hsi->wake_sleep), hsi->uport);

	return (atomic_read(&hsi->wake_sleep) && (hsi->uport!=NULL));
}

static void h5_tx_sleep_wakeup(void)
{
	if (test_bit(STACK_SLEEP, &flags))
	{
	//	printk("%s(): Realtek to wakeup TX\n", __func__);
		wake_lock(&hsi->wake_lock);
		mod_timer(&h5_tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
		atomic_dec(&(hsi->wake_sleep));
		clear_bit(STACK_SLEEP, &flags);
	}
}
/**
 * @brief@  main sleep work handling function which update the flags
 * and activate and deactivate UART ,check FIFO.
 */
static void h5_sleep_work(struct work_struct *work)
{
	if (h5_can_sleep()) {
	//	printk("Realtek: can sleep...\n");
		/* already asleep, this is an error case */
		if (test_bit(STACK_SLEEP, &flags)) {
			printk("Realtek: error, already asleep");
			return;
		}

		if (hsi->uport->ops->tx_empty(hsi->uport)) {
	//		printk("going to sleep...\n");
			set_bit(STACK_SLEEP , &flags);
			/* UART clk is not turned off immediately. Release
			 * wakelock after 500 ms.
			 */
			wake_lock_timeout(&hsi->wake_lock, HZ);
		} else {
	//		printk("tx buffer is not empty, modify timer...\n");
			/*lgh add*/
			atomic_dec(&hsi->wake_sleep);
			/*lgh add end*/
			//mod_timer(&h5_tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
			mod_timer(&h5_tx_timer, jiffies + ( HZ / 4));
			return;
		}
	}else {
		h5_tx_sleep_wakeup();
	}
}


/**
 * Handles transmission timer expiration.
 * @param data Not used.
 */
static void h5_sleep_tx_timer_expire(unsigned long data)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&rw_lock, irq_flags);

//	printk("Realtek: Tx timer expired\n");

	/* were we silent during the last timeout? */
	if (!test_bit(STACK_TXDATA, &flags)) {
	//	printk("Realtek: Tx has been idle\n");
		atomic_inc(&(hsi->wake_sleep));
		schedule_delayed_work(&sleep_workqueue, 0);
	} else {
	//	printk("Realtek: Tx data during last period\n");
	//	mod_timer(&h5_tx_timer, jiffies + (TX_TIMER_INTERVAL*HZ));
		mod_timer(&h5_tx_timer, jiffies + (HZ / 4));
	}

	/* clear the incoming data flag */
	clear_bit(STACK_TXDATA, &flags);

	spin_unlock_irqrestore(&rw_lock, irq_flags);
}


static void h5_sleep_stack_xmit_data(void)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&rw_lock, irq_flags);

	set_bit(STACK_TXDATA, &flags);

	/*if tx is going to sleep
	 */
	if (atomic_read(&(hsi->wake_sleep)) == 1){
	//	printk("TX  was sleeping\n");
		h5_tx_sleep_wakeup();
	}

	spin_unlock_irqrestore(&rw_lock, irq_flags);
}

static int h5_parse_hci_event(struct notifier_block *this,
		unsigned long event, void *data)
{
	struct hci_dev *hdev = (struct hci_dev *) data;
	struct hci_uart *hu;
	struct uart_state *state;

	if (!hdev)
		return NOTIFY_DONE;

//	printk("Realtek: hci event %ld\n", event);
	switch (event) {
		case HCI_DEV_REG:
	//		printk("Realtek: hci event %ld hdev = %p\n", event, hdev);
			if (!h5_hci_hdev) {
				h5_hci_hdev = hdev;
				hu  = (struct hci_uart *) hdev->driver_data;
				state = (struct uart_state *) (hu->tty->driver_data);
				hsi->uport = state->uart_port;
				mod_timer(&h5_tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
				wake_lock(&hsi->wake_lock);
			}

	//		printk("Realtek: start check\n");
#if 0
			if(need_check_bt_state)
				schedule_delayed_work(&bt_state_check_work, HZ * 60);
#endif
			break;
		case HCI_DEV_UNREG:
	//		printk("Realtek: hci event %ld\n", event);

			del_timer(&h5_tx_timer);
			wake_lock_timeout(&hsi->wake_lock, HZ);
			h5_hci_hdev = NULL;
			hsi->uport = NULL;
	//		printk("Realtek: stop check\n");
#if 0
			if(need_check_bt_state)
				cancel_delayed_work(&bt_state_check_work);
#endif
			break;

		case HCI_DEV_WRITE:
			h5_sleep_stack_xmit_data();
			break;
	}

	return NOTIFY_DONE;
}

/*
 * h5 sleep function init
 */
static void h5_sleep_init(void)
{
	flags = 0; /* clear all status bits */

	hsi = kzalloc(sizeof(struct h5_sleep_info), GFP_KERNEL);
	if (hsi==NULL)
	{
		BT_ERR("alllocate mem for h5_sleep fail?\n");
		return ;
	}

	memset(hsi,0, sizeof(struct h5_sleep_info));

	/* Initialize spinlock. */
	spin_lock_init(&rw_lock);

	/* Initialize timer */
	init_timer(&h5_tx_timer);
	h5_tx_timer.function = h5_sleep_tx_timer_expire;
	h5_tx_timer.data = 0;

	atomic_set(&hsi->wake_sleep, 0);
	hci_register_notifier(&hci_event_nblock);


	wake_lock_init(&hsi->wake_lock, WAKE_LOCK_SUSPEND, "h5_sleep");

}
/*
   h5 sleep function exit
 */
static void h5_sleep_exit(void)
{
	hci_unregister_notifier(&hci_event_nblock);
	del_timer(&h5_tx_timer);
	wake_lock_destroy(&hsi->wake_lock);
	kfree(hsi);
}
//for sleep check end
#endif

static int h5_open(struct hci_uart *hu)
{
	struct h5_struct *h5;

	BT_DBG("hu %p", hu);

	h5 = kzalloc(sizeof(*h5), GFP_ATOMIC);
	if (!h5)
		return -ENOMEM;

	hu->priv = h5;
	skb_queue_head_init(&h5->unack);
	skb_queue_head_init(&h5->rel);
	skb_queue_head_init(&h5->unrel);

	init_timer(&h5->th5);
	h5->th5.function = h5_timed_event;
	h5->th5.data     = (u_long) hu;

	h5_config_state_check();
	printk("Realtek need check bt state: %d\n", need_check_bt_state);
	if (need_check_bt_state) {
		mutex_init(&sem_exit);
		hci_uart_info = hu;

		schedule_delayed_work(&bt_state_check_work, HZ * 60);

	}

	h5->rx_state = H5_W4_PKT_DELIMITER;

	if (txcrc)
		h5->use_crc = 1;

	return 0;
}

static int h5_close(struct hci_uart *hu)
{
	struct h5_struct *h5 = hu->priv;
//	hu->priv = NULL;

	BT_DBG("hu %p", hu);

	if (need_check_bt_state) {
		int ret = 0;
		cancel_delayed_work(&bt_state_check_work);
		cancel_delayed_work(&bt_state_err_work);
		ret = mutex_lock_interruptible(&sem_exit); 	//wait work queue perform completely
		if (ret != 0)
		{
			printk("Realtek mutex unlocked:%x, %s()\n", ret, __func__);
		}
		hci_uart_info = NULL;
	}
	skb_queue_purge(&h5->unack);
	skb_queue_purge(&h5->rel);
	skb_queue_purge(&h5->unrel);
	del_timer(&h5->th5);

	hu->priv = NULL;
	kfree(h5);
	return 0;
}

static struct hci_uart_proto h5 = {
	.id		= HCI_UART_3WIRE,
	.open		= h5_open,
	.close		= h5_close,
	.enqueue	= h5_enqueue,
	.dequeue	= h5_dequeue,
	.recv		= h5_recv,
	.flush		= h5_flush
};

int h5_init(void)
{
	int err = hci_uart_register_proto(&h5);

	BT_INFO("Realtek h5 driver version %s", VERSION);
	if (!err)
		BT_INFO("HCI Realtek H5 protocol initialized");
	else
		BT_ERR("HCI Realtek H5 protocol registration failed");

#ifdef RTK_BLUE_SLEEP
	//for sleep check
	h5_sleep_init();
#endif

	return err;
}

int h5_deinit(void)
{
#ifdef RTK_BLUE_SLEEP
	//for sleeep check
	h5_sleep_exit();
#endif
	return hci_uart_unregister_proto(&h5);
}
