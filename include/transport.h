#ifndef ADB_TRANSPORT_H_
#define ADB_TRANSPORT_H_

#include "adb.h"

/* Called from adb_transport_process() when a complete, validated packet
 * has been received. pkt is a pointer to static internal storage —
 * copy out anything you need to keep before returning. */
typedef void (*adb_transport_rx_cb_t)(const struct adb_packet *pkt);

/* Initializes the dedicated ADB UART (lpuart0) and enables RX interrupts.
 * Returns 0 on success, -ENODEV if the UART device isn't ready. */
int adb_transport_init(adb_transport_rx_cb_t rx_cb);

/* Encodes pkt and writes it out over UART (blocking, poll-based TX).
 * Returns 0 on success, or the negative error from adb_encode(). */
int adb_transport_send(const struct adb_packet *pkt);

/* Drains any bytes collected by the RX ISR, feeds the packet parser,
 * and invokes the rx callback for each complete packet found.
 * Call this periodically (main loop or dedicated thread). Blocks up to
 * 50ms waiting for data; returns immediately if none arrives. */
void adb_transport_process(void);

#endif /* ADB_TRANSPORT_H_ */
