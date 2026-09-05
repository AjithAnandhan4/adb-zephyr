#ifndef ADB_H_
#define ADB_H_

#include <stdint.h>
#include <stddef.h>

#define ADB_MAX_PAYLOAD    4096u
#define ADB_HEADER_SIZE    24u   /* 6 x uint32_t, on the wire */

#define ADB_CMD_CNXN 0x4E584E43u
#define ADB_CMD_AUTH 0x48545541u
#define ADB_CMD_OPEN 0x4E45504Fu
#define ADB_CMD_OKAY 0x59414B4Fu
#define ADB_CMD_CLSE 0x45534C43u
#define ADB_CMD_WRTE 0x45545257u

#define ADB_VERSION        0x01000000u

/* In-memory representation. Instances of this struct are ~4.1KB —
 * NEVER declare one as a local variable. Use static/global storage only. */
struct adb_packet {
	uint32_t command;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t length;
	uint32_t checksum;
	uint32_t magic;
	uint8_t  payload[ADB_MAX_PAYLOAD];
};

/* Sum of payload bytes mod 2^32 (legacy ADB "checksum"). */
uint32_t adb_checksum(const uint8_t *buf, uint32_t len);

/* Fill in checksum/magic for a packet whose command/arg0/arg1/length/payload
 * are already set. Safe to call multiple times. */
void adb_packet_finalize(struct adb_packet *pkt);

/*
 * Serialize pkt into out (caller-provided buffer, out_size bytes).
 * On success, *out_len = ADB_HEADER_SIZE + pkt->length.
 * Returns 0 on success, negative errno on failure:
 *   -EINVAL   bad pointer args
 *   -EMSGSIZE pkt->length > ADB_MAX_PAYLOAD
 *   -ENOSPC   out_size too small
 */
int adb_encode(const struct adb_packet *pkt, uint8_t *out, size_t out_size,
	       size_t *out_len);

/*
 * Parse a wire-format buffer into pkt. Validates magic and checksum.
 * Returns 0 on success, negative errno on failure:
 *   -EINVAL   bad pointer args
 *   -EMSGSIZE in_len too short for header or declared payload length
 *   -EPROTO   magic != ~command
 *   -EBADMSG  checksum mismatch
 */
int adb_decode(struct adb_packet *pkt, const uint8_t *in, size_t in_len);

#endif /* ADB_H_ */
