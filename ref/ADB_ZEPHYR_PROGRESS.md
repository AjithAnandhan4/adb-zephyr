# ADB on Zephyr (FRDM-MCXW71) — Progress Log

## Environment
- Zephyr: v4.4.0-14283-g62acbd571c72 (4.4.99)
- Board: frdm_mcxw71 (qualifier mcxw716c)
- SDK: Zephyr SDK 1.0.1
- Workspace: ~/zephyrproject/zephyr
- App: ~/zephyrproject/adb-zephyr
- Console: lpuart1 @115200, /dev/ttyACM0 (confirmed working via zephyr,console)

## Target architecture
```
PC → transport → ADB packet framing → ADB protocol → Zephyr shell/fs/etc.
```
Roadmap:
1. [DONE] ADB packet framing
2. Transport (UART)
3. CNXN handshake
4. adb devices
5. adb shell → Zephyr shell
6. adb push/pull
7. reboot/state
8. firmware update / MCUboot
9. auth/security

## Milestone 1: Packet layer — STATUS: DONE, VERIFIED ON HARDWARE

### Bug found in original implementation
Original `adb_encode()` used a stack-local buffer:
```c
uint8_t encoded[24 + ADB_MAX_PAYLOAD]; // 4120 bytes
```
`frdm_mcxw71`'s main stack is far smaller than this. Result: stack overflow →
silent corruption of adjacent RAM (heap/.bss/other stacks) → no fault message,
no console output, board appears dead. Not a crash, a corruption.

Secondary issue: encoding via direct `struct` copy assumes no compiler padding
and matches endianness — undefined/implementation-defined behavior in C, even
though it happened to work by luck on Cortex-M today.

### Fix
- All packet-sized buffers (`struct adb_packet`, wire buffer) declared
  `static` → live in `.bss`, not the call stack.
- Manual little-endian serialization via `put_le32`/`get_le32` — no reliance
  on struct layout or host endianness.
- Bounds checking on all encode/decode paths (`ADB_MAX_PAYLOAD`, buffer
  sizes, declared length vs actual input length).
- Magic/checksum validated on decode.

### Verified output (actual hardware run)
```
*** Booting Zephyr OS build v4.4.0-14283-g62acbd571c72 ***
=== ADB packet layer test ===
Encoded CNXN packet: 40 bytes
Decoded header: command=0x4e584e43 arg0=0x01000000 arg1=0x00001000 length=16 checksum=0x00000628 magic=0xb1a7b1bc
PASS: CNXN packet round-trip OK
Payload (16 bytes): "host::adb-zephyr"
```
Round-trip encode→decode confirmed correct on target.

## Current files

### include/adb.h
```c
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
```

### src/packet.c
```c
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
```

### src/main.c
```c
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "adb.h"

/* All packet-sized storage is static (BSS), never on the stack. */
static struct adb_packet tx_pkt;
static struct adb_packet rx_pkt;
static uint8_t wire_buf[ADB_HEADER_SIZE + ADB_MAX_PAYLOAD];

static void build_cnxn_packet(struct adb_packet *pkt)
{
	static const char payload[] = "host::adb-zephyr";
	size_t payload_len = sizeof(payload) - 1; /* exclude NUL terminator */

	memset(pkt, 0, sizeof(*pkt));
	pkt->command = ADB_CMD_CNXN;
	pkt->arg0 = ADB_VERSION;
	pkt->arg1 = ADB_MAX_PAYLOAD;
	pkt->length = (uint32_t)payload_len;

	memcpy(pkt->payload, payload, payload_len);

	adb_packet_finalize(pkt);
}

int main(void)
{
	size_t encoded_len = 0;
	int rc;

	printk("=== ADB packet layer test ===\n");

	build_cnxn_packet(&tx_pkt);

	rc = adb_encode(&tx_pkt, wire_buf, sizeof(wire_buf), &encoded_len);
	if (rc != 0) {
		printk("FAIL: adb_encode() returned %d\n", rc);
		goto done;
	}
	printk("Encoded CNXN packet: %u bytes\n", (unsigned int)encoded_len);

	rc = adb_decode(&rx_pkt, wire_buf, encoded_len);
	if (rc != 0) {
		printk("FAIL: adb_decode() returned %d\n", rc);
		goto done;
	}

	printk("Decoded header: command=0x%08x arg0=0x%08x arg1=0x%08x "
	       "length=%u checksum=0x%08x magic=0x%08x\n",
	       rx_pkt.command, rx_pkt.arg0, rx_pkt.arg1,
	       rx_pkt.length, rx_pkt.checksum, rx_pkt.magic);

	if (rx_pkt.command != tx_pkt.command ||
	    rx_pkt.arg0 != tx_pkt.arg0 ||
	    rx_pkt.arg1 != tx_pkt.arg1 ||
	    rx_pkt.length != tx_pkt.length ||
	    rx_pkt.checksum != tx_pkt.checksum ||
	    rx_pkt.magic != tx_pkt.magic ||
	    memcmp(rx_pkt.payload, tx_pkt.payload, tx_pkt.length) != 0) {
		printk("FAIL: round-trip mismatch\n");
	} else {
		printk("PASS: CNXN packet round-trip OK\n");
		printk("Payload (%u bytes): \"%.*s\"\n",
		       rx_pkt.length, (int)rx_pkt.length, rx_pkt.payload);
	}

done:
	while (1) {
		printk("adb packet test idle\n");
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
```

### CMakeLists.txt (unchanged)
```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(adb_zephyr)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
    src/main.c
    src/packet.c
)
```

### prj.conf (unchanged, optional stack margin)
```
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
# Optional, not required for milestone 1 (buffers are static now):
# CONFIG_MAIN_STACK_SIZE=2048
```

## Key rule going forward
Never put an `ADB_MAX_PAYLOAD`-sized (or `struct adb_packet`-sized) object on
the stack — no local variables, no pass-by-value, no return-by-value. Always
`static`/global storage or heap, passed by pointer.

## Next milestone: UART Transport
Goal: read/write raw bytes over UART, feed into `adb_encode`/`adb_decode`.
- Decide: reuse console UART (lpuart1) vs dedicated UART for ADB traffic.
- Byte-stream framing: read header (24 bytes) first, then read `length` bytes
  of payload — never assume a full packet arrives in one UART callback.
- Use Zephyr UART async or interrupt-driven API with a static RX ring
  buffer (again, not stack-allocated).
- Then: CNXN handshake state machine (milestone 3).

Not started yet — waiting on explicit go-ahead before writing transport code.
