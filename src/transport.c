#include "transport.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <errno.h>

/* Dedicated ADB UART. lpuart1 stays free for console/shell/debug. */
#define ADB_UART_NODE DT_NODELABEL(lpuart0)

static const struct device *const adb_uart_dev = DEVICE_DT_GET(ADB_UART_NODE);

/* Raw byte staging between ISR and process thread. Power-of-2 size. */
#define RX_RINGBUF_SIZE 512
RING_BUF_DECLARE(rx_ringbuf, RX_RINGBUF_SIZE);

static struct k_sem rx_sem;

/* Packet reassembly state. All static — never on the stack. */
static uint8_t parse_buf[ADB_HEADER_SIZE + ADB_MAX_PAYLOAD];
static size_t parse_len;
static size_t parse_target = ADB_HEADER_SIZE;

/* TX scratch buffer. Static for the same reason. */
static uint8_t tx_wire_buf[ADB_HEADER_SIZE + ADB_MAX_PAYLOAD];

/* Decoded-packet storage handed to the user callback. */
static struct adb_packet rx_pkt_storage;

static adb_transport_rx_cb_t user_rx_cb;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		uint8_t byte;

		while (uart_fifo_read(dev, &byte, 1) == 1) {
			(void)ring_buf_put(&rx_ringbuf, &byte, 1);
		}
		k_sem_give(&rx_sem);
	}
}

int adb_transport_init(adb_transport_rx_cb_t rx_cb)
{
	if (!device_is_ready(adb_uart_dev)) {
		return -ENODEV;
	}

	user_rx_cb = rx_cb;
	parse_len = 0;
	parse_target = ADB_HEADER_SIZE;

	k_sem_init(&rx_sem, 0, 1);

	uart_irq_callback_user_data_set(adb_uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(adb_uart_dev);

	return 0;
}

int adb_transport_send(const struct adb_packet *pkt)
{
	size_t out_len;
	int rc;

	if (pkt == NULL) {
		return -EINVAL;
	}

	rc = adb_encode(pkt, tx_wire_buf, sizeof(tx_wire_buf), &out_len);
	if (rc != 0) {
		return rc;
	}

	/* Poll-based TX: simple and sufficient for now. RX stays
	 * interrupt-driven. A TX-interrupt path can be added later if
	 * push/pull throughput demands it. */
	for (size_t i = 0; i < out_len; i++) {
		uart_poll_out(adb_uart_dev, tx_wire_buf[i]);
	}

	return 0;
}

static void parser_reset(void)
{
	parse_len = 0;
	parse_target = ADB_HEADER_SIZE;
}

static void parser_feed(uint8_t byte)
{
	parse_buf[parse_len++] = byte;

	/* Header just completed: decode the length field to learn how
	 * many payload bytes to expect next. */
	if (parse_len == ADB_HEADER_SIZE && parse_target == ADB_HEADER_SIZE) {
		uint32_t length = (uint32_t)parse_buf[12]
				 | ((uint32_t)parse_buf[13] << 8)
				 | ((uint32_t)parse_buf[14] << 16)
				 | ((uint32_t)parse_buf[15] << 24);

		if (length > ADB_MAX_PAYLOAD) {
			/* Garbage on the wire: drop and resync. */
			parser_reset();
			return;
		}

		parse_target = ADB_HEADER_SIZE + length;

		if (parse_target != parse_len) {
			return;
		}
		/* length == 0: header alone is already a complete packet,
		 * fall through to decode below. */
	} else if (parse_len < parse_target) {
		return;
	}

	{
		int rc = adb_decode(&rx_pkt_storage, parse_buf, parse_len);

		if (rc == 0 && user_rx_cb != NULL) {
			user_rx_cb(&rx_pkt_storage);
		}
		/* On decode error (bad checksum/magic) we silently drop
		 * and resync. Add logging here once the log backend
		 * isn't shared with this UART. */
	}

	parser_reset();
}

void adb_transport_process(void)
{
	uint8_t byte;

	if (k_sem_take(&rx_sem, K_MSEC(50)) != 0) {
		return;
	}

	while (ring_buf_get(&rx_ringbuf, &byte, 1) == 1) {
		parser_feed(byte);
	}
}
