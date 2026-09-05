#include "adb.h"
#include <errno.h>
#include <string.h>

uint32_t adb_checksum(const uint8_t *buf, uint32_t len)
{
	uint32_t sum = 0;

	if (buf == NULL) {
		return 0;
	}

	for (uint32_t i = 0; i < len; i++) {
		sum += buf[i];
	}

	return sum;
}

void adb_packet_finalize(struct adb_packet *pkt)
{
	if (pkt == NULL) {
		return;
	}

	if (pkt->length > ADB_MAX_PAYLOAD) {
		pkt->length = ADB_MAX_PAYLOAD;
	}

	pkt->checksum = adb_checksum(pkt->payload, pkt->length);
	pkt->magic = pkt->command ^ 0xFFFFFFFFu;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

int adb_encode(const struct adb_packet *pkt, uint8_t *out, size_t out_size,
	       size_t *out_len)
{
	size_t total;

	if (pkt == NULL || out == NULL || out_len == NULL) {
		return -EINVAL;
	}

	if (pkt->length > ADB_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	total = (size_t)ADB_HEADER_SIZE + pkt->length;
	if (out_size < total) {
		return -ENOSPC;
	}

	put_le32(out + 0,  pkt->command);
	put_le32(out + 4,  pkt->arg0);
	put_le32(out + 8,  pkt->arg1);
	put_le32(out + 12, pkt->length);
	put_le32(out + 16, pkt->checksum);
	put_le32(out + 20, pkt->magic);

	if (pkt->length > 0) {
		memcpy(out + ADB_HEADER_SIZE, pkt->payload, pkt->length);
	}

	*out_len = total;
	return 0;
}

int adb_decode(struct adb_packet *pkt, const uint8_t *in, size_t in_len)
{
	uint32_t command, arg0, arg1, length, checksum, magic, computed;

	if (pkt == NULL || in == NULL) {
		return -EINVAL;
	}

	if (in_len < ADB_HEADER_SIZE) {
		return -EMSGSIZE;
	}

	command  = get_le32(in + 0);
	arg0     = get_le32(in + 4);
	arg1     = get_le32(in + 8);
	length   = get_le32(in + 12);
	checksum = get_le32(in + 16);
	magic    = get_le32(in + 20);

	if (length > ADB_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	if (in_len < (size_t)ADB_HEADER_SIZE + length) {
		return -EMSGSIZE;
	}

	if (magic != (command ^ 0xFFFFFFFFu)) {
		return -EPROTO;
	}

	pkt->command  = command;
	pkt->arg0     = arg0;
	pkt->arg1     = arg1;
	pkt->length   = length;
	pkt->checksum = checksum;
	pkt->magic    = magic;

	if (length > 0) {
		memcpy(pkt->payload, in + ADB_HEADER_SIZE, length);
	}

	computed = adb_checksum(pkt->payload, length);
	if (computed != checksum) {
		return -EBADMSG;
	}

	return 0;
}
