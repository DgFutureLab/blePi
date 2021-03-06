/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011-2014  Intel Corporation
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "src/shared/btsnoop.h"
#include "monitor/bt.h"
#include "analyze.h"

#define MAX_PACKET_SIZE		(1486 + 4)

struct hci_dev {
	uint16_t index;
	uint8_t type;
	uint8_t bdaddr[6];
	struct timeval time_added;
	struct timeval time_removed;
	unsigned long num_cmd;
	unsigned long num_evt;
	unsigned long num_acl;
	unsigned long num_sco;
};

static struct queue *dev_list;

static void dev_destroy(void *data)
{
	struct hci_dev *dev = data;
	const char *str;

	switch (dev->type) {
	case 0x00:
		str = "BR/EDR";
		break;
	case 0x01:
		str = "AMP";
		break;
	default:
		str = "unknown";
		break;
	}

	printf("Found %s controller with index %u\n", str, dev->index);
	printf("  BD_ADDR %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X\n",
			dev->bdaddr[5], dev->bdaddr[4], dev->bdaddr[3],
			dev->bdaddr[2], dev->bdaddr[1], dev->bdaddr[0]);
	printf("  %lu commands\n", dev->num_cmd);
	printf("  %lu events\n", dev->num_evt);
	printf("  %lu ACL packets\n", dev->num_acl);
	printf("  %lu SCO packets\n", dev->num_sco);
	printf("\n");

	free(dev);
}

static struct hci_dev *dev_alloc(uint16_t index)
{
	struct hci_dev *dev;

	dev = new0(struct hci_dev, 1);
	if (!dev) {
		fprintf(stderr, "Failed to allocate new device entry\n");
		return NULL;
	}

	dev->index = index;

	return dev;
}

static bool dev_match_index(const void *a, const void *b)
{
	const struct hci_dev *dev = a;
	uint16_t index = PTR_TO_UINT(b);

	return dev->index == index;
}

static struct hci_dev *dev_lookup(uint16_t index)
{
	struct hci_dev *dev;

	dev = queue_find(dev_list, dev_match_index, UINT_TO_PTR(index));
	if (!dev) {
		fprintf(stderr, "Creating new device for unknown index\n");

		dev = dev_alloc(index);
		if (!dev)
			return NULL;

		queue_push_tail(dev_list, dev);
	}

	return dev;
}

static void new_index(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct btsnoop_opcode_new_index *ni = data;
	struct hci_dev *dev;

	dev = dev_alloc(index);
	if (!dev)
		return;

	dev->type = ni->type;
	memcpy(dev->bdaddr, ni->bdaddr, 6);

	queue_push_tail(dev_list, dev);
}

static void del_index(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	struct hci_dev *dev;

	dev = queue_remove_if(dev_list, dev_match_index, UINT_TO_PTR(index));
	if (!dev) {
		fprintf(stderr, "Remove for an unexisting device\n");
		return;
	}

	dev_destroy(dev);
}

static void command_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct bt_hci_cmd_hdr *hdr = data;
	struct hci_dev *dev;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_cmd++;
}

static void rsp_read_bd_addr(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_rsp_read_bd_addr *rsp = data;

	printf("Read BD Addr event with status 0x%2.2x\n", rsp->status);

	if (rsp->status)
		return;

	memcpy(dev->bdaddr, rsp->bdaddr, 6);
}

static void evt_cmd_complete(struct hci_dev *dev, struct timeval *tv,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_cmd_complete *evt = data;
	uint16_t opcode;

	data += sizeof(*evt);
	size -= sizeof(*evt);

	opcode = le16_to_cpu(evt->opcode);

	switch (opcode) {
	case BT_HCI_CMD_READ_BD_ADDR:
		rsp_read_bd_addr(dev, tv, data, size);
		break;
	}
}

static void event_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct bt_hci_evt_hdr *hdr = data;
	struct hci_dev *dev;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_evt++;

	switch (hdr->evt) {
	case BT_HCI_EVT_CMD_COMPLETE:
		evt_cmd_complete(dev, tv, data, size);
		break;
	}
}

static void acl_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct bt_hci_acl_hdr *hdr = data;
	struct hci_dev *dev;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_acl++;
}

static void sco_pkt(struct timeval *tv, uint16_t index,
					const void *data, uint16_t size)
{
	const struct bt_hci_sco_hdr *hdr = data;
	struct hci_dev *dev;

	data += sizeof(*hdr);
	size -= sizeof(*hdr);

	dev = dev_lookup(index);
	if (!dev)
		return;

	dev->num_sco++;
}

void analyze_trace(const char *path)
{
	struct btsnoop *btsnoop_file;
	unsigned long num_packets = 0;
	uint32_t type;

	btsnoop_file = btsnoop_open(path, BTSNOOP_FLAG_PKLG_SUPPORT);
	if (!btsnoop_file)
		return;

	type = btsnoop_get_type(btsnoop_file);

	switch (type) {
	case BTSNOOP_TYPE_HCI:
	case BTSNOOP_TYPE_UART:
	case BTSNOOP_TYPE_MONITOR:
		break;
	default:
		fprintf(stderr, "Unsupported packet format\n");
		goto done;
	}

	dev_list = queue_new();
	if (!dev_list) {
		fprintf(stderr, "Failed to allocate device list\n");
		goto done;
	}

	while (1) {
		unsigned char buf[MAX_PACKET_SIZE];
		struct timeval tv;
		uint16_t index, opcode, pktlen;

		if (!btsnoop_read_hci(btsnoop_file, &tv, &index, &opcode,
								buf, &pktlen))
			break;

		switch (opcode) {
		case BTSNOOP_OPCODE_NEW_INDEX:
			new_index(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_DEL_INDEX:
			del_index(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_COMMAND_PKT:
			command_pkt(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_EVENT_PKT:
			event_pkt(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_ACL_TX_PKT:
		case BTSNOOP_OPCODE_ACL_RX_PKT:
			acl_pkt(&tv, index, buf, pktlen);
			break;
		case BTSNOOP_OPCODE_SCO_TX_PKT:
		case BTSNOOP_OPCODE_SCO_RX_PKT:
			sco_pkt(&tv, index, buf, pktlen);
			break;
		}

		num_packets++;
	}

	printf("Trace contains %lu packets\n\n", num_packets);

	queue_destroy(dev_list, dev_destroy);

done:
	btsnoop_unref(btsnoop_file);
}
