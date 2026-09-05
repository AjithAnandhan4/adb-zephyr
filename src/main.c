#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdbool.h>
#include "adb.h"
#include "transport.h"

static struct adb_packet tx_pkt;
static struct adb_packet last_rx;
static volatile bool rx_got;

static void on_rx(const struct adb_packet *pkt)
{
	memcpy(&last_rx, pkt, sizeof(last_rx));
	rx_got = true;
}

static void build_cnxn_packet(struct adb_packet *pkt)
{
	static const char payload[] = "host::adb-zephyr";
	size_t payload_len = sizeof(payload) - 1;

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
	int rc;

	printk("=== ADB UART transport loopback test ===\n");
	printk("Requires jumper: PTA17 (LPUART0_TX) -> PTA16 (LPUART0_RX)\n");

	rc = adb_transport_init(on_rx);
	if (rc != 0) {
		printk("FAIL: adb_transport_init() returned %d\n", rc);
		while (1) {
			k_sleep(K_SECONDS(5));
		}
	}

	build_cnxn_packet(&tx_pkt);

	rc = adb_transport_send(&tx_pkt);
	if (rc != 0) {
		printk("FAIL: adb_transport_send() returned %d\n", rc);
	} else {
		printk("Sent CNXN packet (%u byte payload) over lpuart0\n",
		       tx_pkt.length);
	}

	for (int i = 0; i < 20 && !rx_got; i++) {
		adb_transport_process();
	}

	if (!rx_got) {
		printk("FAIL: no packet received "
		       "(check PTA17->PTA16 jumper)\n");
	} else if (last_rx.command != tx_pkt.command ||
		   last_rx.arg0 != tx_pkt.arg0 ||
		   last_rx.arg1 != tx_pkt.arg1 ||
		   last_rx.length != tx_pkt.length ||
		   memcmp(last_rx.payload, tx_pkt.payload,
			  tx_pkt.length) != 0) {
		printk("FAIL: received packet mismatch\n");
	} else {
		printk("PASS: transport loopback round-trip OK\n");
		printk("Received payload: \"%.*s\"\n",
		       (int)last_rx.length, last_rx.payload);
	}

	while (1) {
		adb_transport_process();
		printk("transport test idle\n");
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
