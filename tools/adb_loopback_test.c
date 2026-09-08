/*
 * adb_loopback_test.c — USB bulk loopback test for usbd_adb on FRDM-MCXN236.
 *
 * Tests raw USB bulk transport: sends patterns to EP1 OUT and verifies they
 * return byte-for-byte from EP1 IN. Does NOT use the adb tool or ADB protocol.
 *
 * Build:
 *   make   (uses the Makefile in this directory)
 *   — or —
 *   gcc -Wall -Wextra -o adb_loopback_test adb_loopback_test.c \
 *       $(pkg-config --cflags --libs libusb-1.0)
 *
 * Permissions (needed once after flashing):
 *   sudo tee /etc/udev/rules.d/99-zephyr-adb.rules <<'EOF'
 *   SUBSYSTEM=="usb", ATTR{idVendor}=="2fe3", ATTR{idProduct}=="adb1", MODE="0666"
 *   EOF
 *   sudo udevadm control --reload-rules && sudo udevadm trigger
 *   # Unplug and replug J11 USB cable.
 *
 * Run:
 *   ./adb_loopback_test
 */

#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Device identifiers ────────────────────────────────────────────────── */
#define ADB_VID         0x2FE3u
#define ADB_PID         0xADB1u
#define ADB_IFACE       0
#define ADB_EP_OUT      0x01u   /* bulk OUT: host → device */
#define ADB_EP_IN       0x81u   /* bulk IN:  device → host */
#define TIMEOUT_MS      3000    /* per-transfer timeout (ms) */

/* ── Test runner ───────────────────────────────────────────────────────── */

static int g_total;
static int g_passed;
static int g_failed;

static void report(const char *label, bool pass)
{
	g_total++;
	if (pass) {
		g_passed++;
		printf("[PASS] %s\n", label);
	} else {
		g_failed++;
		printf("[FAIL] %s\n", label);
	}
}

/*
 * loopback() — single round-trip: OUT then IN, compare.
 *
 * For payloads that are exact multiples of MPS (64 bytes), the firmware
 * appends a ZLP. libusb_bulk_transfer() returns when it sees the ZLP,
 * so the call completes with exactly tx_len bytes received.
 */
static bool loopback(libusb_device_handle *dev,
		     const char *label,
		     const uint8_t *tx_buf,
		     int tx_len)
{
	uint8_t rx_buf[4096];
	int transferred = 0;
	int ret;

	printf("\n  ── %s (%d bytes) ──\n", label, tx_len);

	if (tx_len <= 0 || tx_len > (int)sizeof(rx_buf)) {
		printf("  SKIP: payload size %d out of range\n", tx_len);
		report(label, false);
		return false;
	}

	/* OUT */
	ret = libusb_bulk_transfer(dev, ADB_EP_OUT,
				   (unsigned char *)tx_buf, tx_len,
				   &transferred, TIMEOUT_MS);
	if (ret != LIBUSB_SUCCESS) {
		printf("  OUT failed: %s (transferred=%d)\n",
		       libusb_error_name(ret), transferred);
		report(label, false);
		return false;
	}
	if (transferred != tx_len) {
		printf("  OUT short: sent=%d expected=%d\n",
		       transferred, tx_len);
		report(label, false);
		return false;
	}
	printf("  OUT: %d bytes sent\n", transferred);

	/* IN */
	memset(rx_buf, 0xCC, (size_t)tx_len);
	transferred = 0;
	ret = libusb_bulk_transfer(dev, ADB_EP_IN,
				   rx_buf, tx_len,
				   &transferred, TIMEOUT_MS);
	if (ret != LIBUSB_SUCCESS) {
		printf("  IN failed: %s (transferred=%d)\n",
		       libusb_error_name(ret), transferred);
		report(label, false);
		return false;
	}
	printf("  IN:  %d bytes received\n", transferred);

	/* Compare */
	if (transferred != tx_len) {
		printf("  Length mismatch: got=%d expected=%d\n",
		       transferred, tx_len);
		report(label, false);
		return false;
	}
	if (memcmp(tx_buf, rx_buf, (size_t)tx_len) != 0) {
		int first = -1;

		for (int i = 0; i < tx_len; i++) {
			if (tx_buf[i] != rx_buf[i]) {
				first = i;
				break;
			}
		}
		printf("  Data mismatch at byte %d: sent=0x%02x got=0x%02x\n",
		       first,
		       (first >= 0) ? (unsigned)tx_buf[first] : 0,
		       (first >= 0) ? (unsigned)rx_buf[first] : 0);
		report(label, false);
		return false;
	}

	report(label, true);
	return true;
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *dev = NULL;
	int ret;

	printf("=== ADB USB bulk loopback test ===\n");
	printf("VID=0x%04X PID=0x%04X  iface=%d  "
	       "EP_OUT=0x%02X  EP_IN=0x%02X  timeout=%dms\n\n",
	       ADB_VID, ADB_PID, ADB_IFACE,
	       ADB_EP_OUT, ADB_EP_IN, TIMEOUT_MS);

	ret = libusb_init(&ctx);
	if (ret != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_init: %s\n", libusb_error_name(ret));
		return 1;
	}
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL,
			  LIBUSB_LOG_LEVEL_WARNING);

	dev = libusb_open_device_with_vid_pid(ctx, ADB_VID, ADB_PID);
	if (!dev) {
		fprintf(stderr,
			"Cannot open %04x:%04x\n\n"
			"Check:\n"
			"  1. J11 USB cable connected to this Linux host\n"
			"  2. Firmware is running (J10 console: 'USB ready')\n"
			"  3. udev rule installed (then unplug/replug J11):\n\n"
			"     sudo tee /etc/udev/rules.d/99-zephyr-adb.rules <<'EOF'\n"
			"     SUBSYSTEM==\"usb\","
			" ATTR{idVendor}==\"2fe3\","
			" ATTR{idProduct}==\"adb1\","
			" MODE=\"0666\"\n"
			"     EOF\n"
			"     sudo udevadm control --reload-rules"
			" && sudo udevadm trigger\n",
			ADB_VID, ADB_PID);
		libusb_exit(ctx);
		return 1;
	}

	if (libusb_kernel_driver_active(dev, ADB_IFACE) == 1) {
		ret = libusb_detach_kernel_driver(dev, ADB_IFACE);
		if (ret != LIBUSB_SUCCESS) {
			fprintf(stderr, "detach kernel driver: %s\n",
				libusb_error_name(ret));
			goto out;
		}
	}

	ret = libusb_claim_interface(dev, ADB_IFACE);
	if (ret != LIBUSB_SUCCESS) {
		fprintf(stderr, "claim interface %d: %s\n",
			ADB_IFACE, libusb_error_name(ret));
		goto out;
	}

	printf("Device opened, interface %d claimed.\n", ADB_IFACE);

	/* ──────────────────────────────────────────────────────────────── */
	/* Test cases                                                        */
	/* ──────────────────────────────────────────────────────────────── */

	/* 1. ASCII string — basic sanity */
	{
		const uint8_t tx[] = "hello";

		loopback(dev, "hello (5 bytes)", tx, (int)sizeof(tx) - 1);
	}

	/* 2. Short ramp < MPS */
	{
		uint8_t tx[37];

		for (int i = 0; i < (int)sizeof(tx); i++) {
			tx[i] = (uint8_t)(i & 0xFF);
		}
		loopback(dev, "short ramp (37 bytes)", tx, (int)sizeof(tx));
	}

	/* 3. Exactly 64 bytes (= FS bulk MPS) — firmware sends ZLP */
	{
		uint8_t tx[64];

		for (int i = 0; i < 64; i++) {
			tx[i] = (uint8_t)(0xA0 + i);
		}
		loopback(dev, "exact MPS=64 bytes (ZLP required)", tx, 64);
	}

	/* 4. 100 bytes — crosses MPS boundary */
	{
		uint8_t tx[100];

		for (int i = 0; i < 100; i++) {
			tx[i] = (uint8_t)(i ^ 0x55);
		}
		loopback(dev, "cross-MPS (100 bytes)", tx, 100);
	}

	/* 5. 128 bytes (2 × MPS) — firmware sends ZLP */
	{
		uint8_t tx[128];

		for (int i = 0; i < 128; i++) {
			tx[i] = (uint8_t)(0xBB ^ i);
		}
		loopback(dev, "2×MPS=128 bytes (ZLP required)", tx, 128);
	}

	/* 6. 256-byte full ramp */
	{
		uint8_t tx[256];

		for (int i = 0; i < 256; i++) {
			tx[i] = (uint8_t)i;
		}
		loopback(dev, "256-byte ramp", tx, 256);
	}

	/* 7. All-zeros */
	{
		uint8_t tx[32];

		memset(tx, 0x00, sizeof(tx));
		loopback(dev, "all-zeros (32 bytes)", tx, (int)sizeof(tx));
	}

	/* 8. All-0xFF */
	{
		uint8_t tx[32];

		memset(tx, 0xFF, sizeof(tx));
		loopback(dev, "all-0xFF (32 bytes)", tx, (int)sizeof(tx));
	}

	/* 9. Single byte */
	{
		uint8_t tx[1] = { 0x42 };

		loopback(dev, "single byte (0x42)", tx, 1);
	}

	/* 10. 63 bytes (MPS-1, no ZLP) */
	{
		uint8_t tx[63];

		for (int i = 0; i < 63; i++) {
			tx[i] = (uint8_t)(0xC0 | (i & 0x3F));
		}
		loopback(dev, "MPS-1=63 bytes (no ZLP)", tx, 63);
	}

	/* 11. Consecutive: 5 × 20-byte back-to-back transfers */
	{
		bool all_ok = true;

		printf("\n  ── consecutive 5×20 bytes ──\n");
		for (int n = 0; n < 5; n++) {
			uint8_t tx[20];
			char label[64];

			for (int i = 0; i < 20; i++) {
				tx[i] = (uint8_t)((n * 20 + i) & 0xFF);
			}
			snprintf(label, sizeof(label),
				 "consecutive[%d] (20 bytes)", n + 1);
			if (!loopback(dev, label, tx, (int)sizeof(tx))) {
				all_ok = false;
			}
		}
		report("consecutive 5×20 bytes (group result)", all_ok);
	}

	/* ──────────────────────────────────────────────────────────────── */
	/* Summary                                                          */
	/* ──────────────────────────────────────────────────────────────── */
	printf("\n══════════════════════════════════════════\n");
	printf("Results: %d/%d passed", g_passed, g_total);
	if (g_failed > 0) {
		printf("  *** %d FAILED ***", g_failed);
	} else {
		printf("  — ALL PASS");
	}
	printf("\n══════════════════════════════════════════\n");

	libusb_release_interface(dev, ADB_IFACE);
out:
	if (dev) {
		libusb_close(dev);
	}
	libusb_exit(ctx);
	return (g_failed == 0) ? 0 : 1;
}
