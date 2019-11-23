/* main.c - Application main entry point */

/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr.h>
#include <misc/printk.h>
#include <drivers/st25r3911b_nfca.h>
#include <nfc/ndef/msg_parser.h>
#include <nfc/t2t/parser.h>
#include <nfc/t4t/isodep.h>
#include <nfc/t4t/apdu.h>

#define NFCA_BD 128
#define BITS_IN_BYTE 8
#define MAX_TLV_BLOCKS 10
#define MAX_NDEF_RECORDS 10
#define NFCA_T2T_BUFFER_SIZE 1024
#define NFCA_LAST_BIT_MASK 0x80
#define NFCA_FDT_ALIGN_84 84
#define NFCA_FDT_ALIGN_20 20

#define NFC_T2T_READ_CMD 0x30
#define NFC_T2T_READ_CMD_LEN 0x02

#define NFC_T4T_ISODEP_FSD 256
#define NFC_T4T_ISODEP_RX_DATA_MAX_SIZE 1024
#define NFC_T4T_APDU_MAX_SIZE 1024

#define NFC_TX_DATA_LEN NFC_T4T_ISODEP_FSD
#define NFC_RX_DATA_LEN NFC_T4T_ISODEP_FSD

#define T2T_MAX_DATA_EXCHANGE 16
#define TAG_TYPE_2_DATA_AREA_MULTIPLICATOR 8
#define TAG_TYPE_2_DATA_AREA_SIZE_OFFSET (NFC_T2T_CC_BLOCK_OFFSET + 2)
#define TAG_TYPE_2_BLOCKS_PER_EXCHANGE (T2T_MAX_DATA_EXCHANGE / NFC_T2T_BLOCK_SIZE)

#define NFC_T4T_APDU_APP_SELECT_DATA  {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01}
#define NFC_T4T_APDU_RSP_ALL 256

#define TRANSMIT_DELAY 3000
#define ALL_REQ_DELAY 2000

static u8_t tx_data[NFC_TX_DATA_LEN];
static u8_t rx_data[NFC_RX_DATA_LEN];

static struct k_poll_event events[ST25R3911B_NFCA_EVENT_CNT];
static struct k_delayed_work transmit_work;

static struct st25r3911b_nfca_buf tx_buf = {
	.data = tx_data,
	.len = sizeof(tx_data)
};

static const struct st25r3911b_nfca_buf rx_buf = {
	.data = rx_data,
	.len = sizeof(rx_data)
};

enum nfc_tag_type {
	NFC_TAG_TYPE_UNSUPPORTED = 0,
	NFC_TAG_TYPE_T2T,
	NFC_TAG_TYPE_T4T
};

enum t2t_state {
	T2T_IDLE,
	T2T_HEADER_READ,
	T2T_DATA_READ
};

struct t2t_tag {
	enum t2t_state state;
	u16_t data_bytes;
	u8_t data[NFCA_T2T_BUFFER_SIZE];
};

struct t4t_tag {
	u8_t data[NFC_T4T_ISODEP_RX_DATA_MAX_SIZE];
	u8_t apdu_raw[NFC_T4T_APDU_MAX_SIZE];
};

static enum nfc_tag_type tag_type;
static struct t2t_tag t2t;
static struct t4t_tag t4t;

static void nfc_tag_detect(bool all_request)
{
	int err;
	enum st25r3911b_nfca_detect_cmd cmd;

	tag_type = NFC_TAG_TYPE_UNSUPPORTED;

	cmd = all_request ? ST25R3911B_NFCA_DETECT_CMD_ALL_REQ :
		ST25R3911B_NFCA_DETECT_CMD_SENS_REQ;

	err = st25r3911b_nfca_tag_detect(cmd);
	if (err) {
		printk("Tag detect error: %d\n", err);
	}
}

static int ftd_calculate(u8_t *data, size_t len)
{
	u8_t ftd_align;

	ftd_align = (data[len - 1] & NFCA_LAST_BIT_MASK) ?
		NFCA_FDT_ALIGN_84 : NFCA_FDT_ALIGN_20;

	return len * NFCA_BD * BITS_IN_BYTE + ftd_align;
}

static int nfc_t4t_tag_app_select(void)
{
	int err;
	struct nfc_t4t_apdu_comm apdu_comm;
	u16_t len = sizeof(t4t.apdu_raw);
	u8_t t4t_app_name[] = NFC_T4T_APDU_APP_SELECT_DATA;

	nfc_t4t_apdu_comm_clear(&apdu_comm);

	apdu_comm.instruction = NFC_T4T_APDU_COMM_INS_SELECT;
	apdu_comm.parameter = NFC_T4T_APDU_SELECT_BY_NAME;
	apdu_comm.data.buff = t4t_app_name;
	apdu_comm.data.len = sizeof(t4t_app_name);
	apdu_comm.resp_len = NFC_T4T_APDU_RSP_ALL;

	err = nfc_t4t_apdu_comm_encode(&apdu_comm,
				       t4t.apdu_raw,
				       &len);
	if (err) {
		return err;
	}

	return nfc_t4t_isodep_transmit(t4t.apdu_raw, len);
}

static int nfc_t2t_read_block_cmd_make(u8_t *tx_data,
				       size_t tx_data_size,
				       u8_t block_num)
{
	if (!tx_data) {
		return -EINVAL;
	}

	if (tx_data_size < NFC_T2T_READ_CMD_LEN) {
		return -ENOMEM;
	}

	tx_data[0] = NFC_T2T_READ_CMD;
	tx_data[1] = block_num;

	return 0;
}

static int t2t_header_read(void)
{
	int err;
	int ftd;

	err = nfc_t2t_read_block_cmd_make(tx_data, sizeof(tx_data), 0);
	if (err) {
		return err;
	}

	tx_buf.data = tx_data;
	tx_buf.len = NFC_T2T_READ_CMD_LEN;

	ftd = ftd_calculate(tx_data, NFC_T2T_READ_CMD_LEN);

	t2t.state = T2T_HEADER_READ;

	err = st25r3911b_nfca_transfer_with_crc(&tx_buf, &rx_buf, ftd);

	return err;
}

static void ndef_data_analyze(const u8_t *ndef_msg_buff, size_t nfc_data_len)
{
	int  err;
	u8_t desc_buf[NFC_NDEF_PARSER_REQIRED_MEMO_SIZE_CALC(MAX_NDEF_RECORDS)];
	size_t desc_buf_len = sizeof(desc_buf);

	err = nfc_ndef_msg_parse(desc_buf,
				 &desc_buf_len,
				 ndef_msg_buff,
				 &nfc_data_len);
	if (err) {
		printk("Error during parsing a NDEF message, err: %d.\n", err);
	}

	nfc_ndef_msg_printout((struct nfc_ndef_msg_desc *) desc_buf);
}

static void t2t_data_read_complete(u8_t *data)
{
	int err;

	if (!data) {
		printk("No T2T data read.\n");
		return;
	}

	/* Declaration of Type 2 Tag structure. */
	NFC_T2T_DESC_DEF(tag_data, MAX_TLV_BLOCKS);
	struct nfc_t2t *t2t = &NFC_T2T_DESC(tag_data);

	err = nfc_t2t_parse(t2t, data);
	if (err) {
		printk("Not enought memory to read whole tag. Printing what have been read.\n");
	}

	nfc_t2t_printout(t2t);

	struct nfc_t2t_tlv_block *tlv_block = t2t->tlv_block_array;

	for (size_t i = 0; i < t2t->tlv_count; i++) {
		if (tlv_block->tag == NFC_T2T_TLV_NDEF_MESSAGE) {
			ndef_data_analyze(tlv_block->value, tlv_block->length);
			tlv_block++;
		}
	}

	st25r3911b_nfca_tag_sleep();

	k_delayed_work_submit(&transmit_work, TRANSMIT_DELAY);
}

static int t2t_on_data_read(const u8_t *data, size_t data_len,
			    void (*t2t_read_complete)(u8_t *))
{
	int err;
	int ftd;
	u8_t block_to_read;
	u16_t offset = 0;
	static u8_t block_num;

	block_to_read = t2t.data_bytes / NFC_T2T_BLOCK_SIZE;
	offset = block_num * NFC_T2T_BLOCK_SIZE;

	memcpy(t2t.data + offset, data, data_len);

	block_num += TAG_TYPE_2_BLOCKS_PER_EXCHANGE;

	if (block_num > block_to_read) {
		block_num = 0;
		t2t.state = T2T_IDLE;

		if (t2t_read_complete) {
			t2t_read_complete(t2t.data);
		}

		return 0;
	}

	err = nfc_t2t_read_block_cmd_make(tx_data, sizeof(tx_data), block_num);
	if (err) {
		return err;
	}

	tx_buf.data = tx_data;
	tx_buf.len = NFC_T2T_READ_CMD_LEN;

	ftd = ftd_calculate(tx_data, NFC_T2T_READ_CMD_LEN);

	err = st25r3911b_nfca_transfer_with_crc(&tx_buf, &rx_buf, ftd);

	return err;
}

static int on_t2t_transfer_complete(const u8_t *data, size_t len)
{
	switch (t2t.state) {
	case T2T_HEADER_READ:
		t2t.data_bytes = TAG_TYPE_2_DATA_AREA_MULTIPLICATOR *
				 data[TAG_TYPE_2_DATA_AREA_SIZE_OFFSET];

		if ((t2t.data_bytes + NFC_T2T_FIRST_DATA_BLOCK_OFFSET) > sizeof(t2t.data)) {
			return -ENOMEM;
		}

		t2t.state = T2T_DATA_READ;

		return t2t_on_data_read(data, len, t2t_data_read_complete);

	case T2T_DATA_READ:
		return t2t_on_data_read(data, len, t2t_data_read_complete);

	default:
		return -EFAULT;
	}
}

static void transfer_handler(struct k_work *work)
{
	nfc_tag_detect(false);
}

static void nfc_field_on(void)
{
	printk("NFC field on\n");

	nfc_tag_detect(false);
}

static void nfc_timeout(bool tag_sleep)
{
	if (tag_sleep) {
		printk("Tag sleep or no detected, sending ALL Request\n");
	} else if (tag_type == NFC_TAG_TYPE_T4T) {
		nfc_t4t_isodep_on_timeout();

		return;
	} else {
		/* Do nothing. */
	}

	/* Sleep will block processing loop. Accepted as it is short. */
	k_sleep(ALL_REQ_DELAY);

	nfc_tag_detect(true);
}

static void nfc_field_off(void)
{
	printk("NFC field off\n");
}

static void tag_detected(const struct st25r3911b_nfca_sens_resp *sens_resp)
{
	printk("Anticollision: 0x%x Platform: 0x%x\n",
		sens_resp->anticollison,
		sens_resp->platform_info);

	int err = st25r3911b_nfca_anticollision_start();

	if (err) {
		printk("Anticollision error: %d\n", err);
	}
}

static void anticollision_completed(const struct st25r3911b_nfca_tag_info *tag_info,
				    int err)
{
	if (err) {
		printk("Error during anticollision avoidance.\n");

		nfc_tag_detect(false);

		return;
	}

	printk("Tag info, type: %d\n", tag_info->type);

	if (tag_info->type == ST25R3911B_NFCA_TAG_TYPE_T2T) {
		printk("Type 2 Tag.\n");

		tag_type = NFC_TAG_TYPE_T2T;

		err = t2t_header_read();
		if (err) {
			printk("Type 2 Tag data read error %d.\n", err);
		}
	} else if (tag_info->type == ST25R3911B_NFCA_TAG_TYPE_T4T) {
		printk("Type 4 Tag.\n");

		tag_type = NFC_TAG_TYPE_T4T;

		/* Send RATS command */
		err = nfc_t4t_isodep_rats_send(NFC_T4T_ISODEP_FSD_256, 0);
		if (err) {
			printk("Type 4 Tag RATS sending error %d.\n", err);
		}
	} else {
		printk("Unsupported tag type.\n");

		tag_type = NFC_TAG_TYPE_UNSUPPORTED;

		nfc_tag_detect(false);

		return;
	}
}

static void transfer_completed(const u8_t *data, size_t len, int err)
{
	if (err) {
		printk("NFC Transfer error: %d\n", err);
		return;
	}

	switch (tag_type) {
	case NFC_TAG_TYPE_T2T:
		err = on_t2t_transfer_complete(data, len);
		if (err) {
			printk("NFC-A T2T read error: %d.\n", err);
		}

		break;

	case NFC_TAG_TYPE_T4T:
		err = nfc_t4t_isodep_data_received(data, len, err);
		if (err) {
			printk("NFC-A T4T read error: %d.\n", err);
		}

		break;

	default:
		break;
	}
}

static void tag_sleep(void)
{
	printk("Tag entered the Sleep state.\n");
}

static const struct st25r3911b_nfca_cb cb = {
	.field_on = nfc_field_on,
	.field_off = nfc_field_off,
	.tag_detected = tag_detected,
	.anticollision_completed = anticollision_completed,
	.rx_timeout = nfc_timeout,
	.transfer_completed = transfer_completed,
	.tag_sleep = tag_sleep
};

static void t4t_isodep_selected(const struct nfc_t4t_isodep_tag *t4t_tag)
{
	int err;

	printk("NFC T4T selected.\n");

	if ((t4t_tag->lp_divisor != 0) &&
	    (t4t_tag->lp_divisor != t4t_tag->pl_divisor)) {
		printk("Unsupported bitrate divisor by Reader/Writer.\n");
	}

	err = nfc_t4t_tag_app_select();
	if (err) {
		printk("NFC T4T app select err %d.\n", err);
		return;
	}
}

static void t4t_isodep_error(int err)
{
	printk("ISO-DEP Protocol error %d\n.", err);

	nfc_tag_detect(false);
}

static void t4t_isodep_data_send(u8_t *data, size_t data_len, u32_t ftd)
{
	int err;

	tx_buf.data = data;
	tx_buf.len = data_len;

	err = st25r3911b_nfca_transfer_with_crc(&tx_buf, &rx_buf, ftd);
	if (err) {
		printk("NFC T4T ISO-DEP transfer error: %d\n.", err);
	}
}

static void t4t_isodep_received(const u8_t *data, size_t data_len)
{
	int err;
	struct nfc_t4t_apdu_resp apdu_resp;

	nfc_t4t_apdu_resp_clear(&apdu_resp);

	err = nfc_t4t_apdu_resp_decode(&apdu_resp, data, data_len);
	if (err) {
		printk("NFC APDU data decode err %d.\n", err);
		return;
	}

	if (apdu_resp.status == NFC_T4T_APDU_RAPDU_STATUS_CMD_COMPLETED) {
		printk("NFC T4T app selected successfully.\n");
	} else {
		printk("NFC T4T app not selected.\n");
	}

	st25r3911b_nfca_tag_sleep();

	k_delayed_work_submit(&transmit_work, TRANSMIT_DELAY);

}

static const struct nfc_t4t_isodep_cb t4t_isodep_cb = {
	.selected = t4t_isodep_selected,
	.error = t4t_isodep_error,
	.ready_to_send = t4t_isodep_data_send,
	.data_received = t4t_isodep_received
};

void main(void)
{
	int err;

	printk("NFC reader sample started\n");

	k_delayed_work_init(&transmit_work, transfer_handler);

	err = nfc_t4t_isodep_init(tx_data, sizeof(tx_data),
				  t4t.data, sizeof(t4t.data),
				  &t4t_isodep_cb);
	if (err) {
		printk("NFC T4T ISO-DEP Protocol initialization failed err: %d\n",
		       err);
		return;
	}

	err = st25r3911b_nfca_init(events, ARRAY_SIZE(events), &cb);
	if (err) {
		printk("NFCA initialization failed err: %d\n", err);
		return;
	}

	err = st25r3911b_nfca_field_on();
	if (err) {
		printk("Field on error %d", err);
		return;
	}

	while (true) {
		k_poll(events, ARRAY_SIZE(events), K_FOREVER);
		err = st25r3911b_nfca_process();
		if (err) {
			printk("NFC-A process failed, err: %d\n", err);
			return;
		}
	}
}
