# ADB on Zephyr — Project State

**Current goal: an upstream-quality, generic Zephyr USB ADB device
function** usable on any Zephyr-supported SoC with a USB device
controller. FRDM-MCXN236 is the first development/test board, not the
final target (see architecture change below for full detail).

## Environment
- Board: FRDM-MCXW71 (MCXW716C)
- Zephyr: v4.4.0-14283-g62acbd571c72 (4.4.99)
- SDK: Zephyr SDK 1.0.1
- Workspace: ~/zephyrproject/zephyr
- App: ~/zephyrproject/adb-zephyr

## UART assignment
- LPUART1 = console/debug, WORKING, /dev/ttyACM0 @ 115200 (zephyr,console = &lpuart1)
- LPUART0 = dedicated ADB transport (separate from console, per design decision)
  - RX = PTA16, TX = PTA17
  - Already `status = "okay"` in board default DTS, pinctrl already defined
    (`pinmux_lpuart0` in frdm_mcxw71-pinctrl.dtsi) — no overlay needed
  - current-speed already 115200 by board default
- Driver style: interrupt-driven (CONFIG_UART_INTERRUPT_DRIVEN=y), not async/DMA

## Milestone roadmap
1. [DONE] ADB packet framing (encode/decode, checksum, magic)
2. [DONE] Transport (UART, interrupt-driven, loopback test) — see "Abandoned" section below
3. [ABANDONED] Real PC↔MCXW71 CNXN handshake over UART/TCP-serial-bridge
4. [BLOCKED — HARDWARE LIMITATION] Native USB ADB on MCXW71
5. adb shell → Zephyr shell
6. adb push/pull
7. reboot/state
8. firmware update / MCUboot
9. auth/security

## Milestone 1: Packet layer — DONE, VERIFIED ON HARDWARE
Root cause of original failure: 4120-byte buffer on the call stack overflowed
the board's small main stack → silent RAM corruption, no fault message, no
console output. Fix: all packet-sized objects (`struct adb_packet`, wire
buffers) are `static`, never stack-local or pass/return-by-value. Encoding
uses explicit little-endian byte packing (`put_le32`/`get_le32`), not
struct-copy (which relies on undefined/implementation-defined padding and
endianness behavior).

Verified hardware output:
```
=== ADB packet layer test ===
Encoded CNXN packet: 40 bytes
Decoded header: command=0x4e584e43 arg0=0x01000000 arg1=0x00001000 length=16 checksum=0x00000628 magic=0xb1a7b1bc
PASS: CNXN packet round-trip OK
Payload (16 bytes): "host::adb-zephyr"
```

## Milestone 2: UART transport — COMPLETE, VERIFIED ON HARDWARE
Design: interrupt-driven RX on lpuart0 into a ring buffer, drained by
`adb_transport_process()` into a static byte-stream parser that reassembles
full ADB packets (header first, then declared-length payload) and calls a
user RX callback. TX is poll-based for now (simple, sufficient pre-throughput
testing; can move to interrupt-driven TX later if push/pull needs it).

Test setup: loopback jumper PTA17 (LPUART0_TX) → PTA16 (LPUART0_RX), no host
PC or USB-serial adapter needed yet. Board sends a CNXN packet to itself over
lpuart0 and confirms it decodes back correctly.

### Build error hit and fixed
```
error: invalid use of void expression
    if (!uart_irq_update(dev))
```
Cause: in Zephyr 4.4.99, `uart_irq_update()` returns `void`, not `int`
(this differs from older Zephyr versions/API docs assumptions). Fix: call it
unconditionally, without wrapping in a return-value check:

```c
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
```
No other logic changed. Awaiting build confirmation on hardware.

### Loopback test — PASSED on hardware
Actual confirmed output, PTA17→PTA16 jumper, lpuart0:
```
Sent CNXN packet (16 byte payload)
PASS: transport loopback round-trip OK
Received payload: "host::adb-zephyr"
```
Confirms: interrupt-driven RX into ring buffer, byte-stream parser
reassembly (header then declared-length payload), poll-based TX, and
`adb_encode`/`adb_decode` all work correctly together on real hardware over
lpuart0, independent of the console UART (lpuart1).

## Files currently created/modified
```
adb-zephyr/
├── CMakeLists.txt          (adds src/transport.c)
├── prj.conf                (adds CONFIG_UART_INTERRUPT_DRIVEN=y)
├── app.overlay             (unchanged — no overlay needed for lpuart0)
├── include/
│   ├── adb.h               (packet header/API — from milestone 1)
│   └── transport.h         (transport API — new in milestone 2)
└── src/
    ├── main.c              (now: transport loopback test, replaces
    │                         milestone-1 packet-only test)
    ├── packet.c             (unchanged from milestone 1)
    └── transport.c          (new in milestone 2; fixed uart_irq_update() call)
```

## Key rules established so far
- Never put an `ADB_MAX_PAYLOAD`-sized (or `struct adb_packet`-sized) object
  on the stack — static/global storage only, always passed by pointer.
- ADB packet framing/protocol core (`adb.h`/`packet.c`) must stay
  hardware/transport-independent so it can be reused across UART, USB, or
  any future transport without modification.
- Do not guess hardware peripheral support — verify against the actual
  Zephyr source tree and board DTS before writing driver-dependent code.

## PROJECT TARGET CHANGED: FRDM-MCXN236 (was FRDM-MCXW71)

Reason: MCXW71 has no USB device controller in silicon (confirmed by
exhaustive investigation — see "MCXW71 native USB investigation" section
below). Native USB ADB is the production goal, so the board was switched
to FRDM-MCXN236, which does have a real USB device controller.

## Board: FRDM-MCXN236 — USB support VERIFIED (not assumed)

- Board target: `frdm_mcxn236` (single qualifier; confirmed by successful
  build — ELF generation resolved it as `frdm_mcxn236/mcxn236`)
- USB controller: `usb1` @ 0x10b000, `compatible = "nxp,ehci"`, EHCI-style,
  8 bidirectional endpoints, paired with `usbphy1`. Aliased as
  `zephyr_udc0` in board DTS — already enabled, no overlay needed.
- Matching Zephyr driver: `drivers/usb/udc/udc_mcux_ehci.c`
  (`DT_DRV_COMPAT nxp_ehci`) — the **new UDC driver API**
  (`CONFIG_USB_DEVICE_STACK_NEXT`), not the legacy `usb_dc_mcux.c` path.
- **Verified by actual build**: `samples/subsys/usb/cdc_acm` built clean
  for this board (FLASH 6.65%, RAM 9.76%). Resulting `.config` confirms:
  `CONFIG_USB_DEVICE_STACK` not set, `CONFIG_USB_DEVICE_STACK_NEXT=y`,
  `CONFIG_UDC_DRIVER=y`, `CONFIG_UDC_NXP_EHCI=y`.

**Implication:** any ADB USB transport must target the new
`USB_DEVICE_STACK_NEXT`/UDC class API, not legacy `usb_dc_*` calls.

## Why the existing packet layer is reusable as-is
Real `adb` over USB does not use a standard class (not CDC-ACM). The host's
libusb transport discovers the device purely by interface descriptor:
class `0xFF` (vendor-specific), subclass `0x42`, protocol `0x01`, exactly
two bulk endpoints (one IN, one OUT). No control-transfer protocol — raw
ADB packets (24-byte header + payload, same format already implemented in
`packet.c`/`adb.h`) go directly over the bulk endpoints. So:
- `adb.h`/`packet.c`: **no changes needed**.
- New work is a custom vendor-class USB interface (2 bulk endpoints)
  feeding/draining the same `adb_encode`/`adb_decode` functions — same
  shape as the abandoned UART `transport.c`, different byte source/sink.
- `cnxn.c` handshake logic is also reusable unchanged — it only depends on
  a transport callback/send interface, not UART specifics.

## Proposed implementation plan (NOT YET CODED)
1. Enable `CONFIG_USB_DEVICE_STACK_NEXT` + `CONFIG_UDC_NXP_EHCI` in
   `prj.conf` (devicetree already provides `zephyr_udc0`; no overlay
   needed).
2. Define USB device descriptors (VID/PID — placeholder, doesn't affect
   real adb's interface auto-detection).
3. Define one custom interface: class 0xFF, subclass 0x42, protocol 0x01,
   one bulk-IN + one bulk-OUT endpoint, registered via the new stack's
   class-registration API.
4. Bulk-OUT callback feeds bytes into byte-stream reassembly logic (same
   design as the abandoned UART transport's parser, adapted to the new
   stack's buffer/callback model instead of ISR+ring-buffer).
5. Bulk-IN path calls `adb_encode()` into a static buffer, submits as a USB
   transfer.
6. Reuse `cnxn.c` handshake logic unchanged.

## Immediate next step
Before writing code: inspect how Zephyr 4.4.99's new USB stack
(`USB_DEVICE_STACK_NEXT`) samples define a *custom* (non-predefined)
vendor class with raw bulk endpoints — need a real example from this exact
tree to implement descriptor/endpoint registration correctly, not from
memory. This is investigation, not implementation, and comes before any
code changes.

## Next milestone after that
Implement the USB vendor-class ADB transport (descriptors + bulk
endpoints) per the plan above, verify `adb devices` lists the board, then
move to `adb shell` (OPEN/WRTE/OKAY/CLSE stream multiplexing on top of the
now-USB-based CNXN connection).

---

# MCXW71 native USB investigation (superseded — board changed)

MCXW71 does not implement a USB device controller in silicon:
- No USB node anywhere in the SoC devicetree (`dts/arm/nxp/mcx/mcxw/`).
- No USB device-controller driver in Zephyr's tree references this SoC
  family (`mcxw`/`kw3x`).
- No USB module in the NXP HAL for MCXW71.
- The board's only USB reference is the onboard debug probe (J10), a
  separate MCU-Link/CMSIS-DAP chip providing the virtual COM port
  (`/dev/ttyACM0` = lpuart1) — not a USB peripheral wired to the MCXW71
  itself.

This is a hardware limitation, not a Zephyr driver gap, and is why the
project target changed to FRDM-MCXN236.

**Status of work done before abandonment: UART loopback milestone was
successfully completed** (see Milestone 2 above — verified on hardware,
PASS, real console output captured). The packet layer (milestone 1) and
UART transport (milestone 2) both worked correctly. Milestone 3 (CNXN
handshake over UART + PC-side TCP↔serial bridge) was implemented in code
but never verified on hardware before the decision below was made.

**Reason for abandonment:** the production goal is native USB ADB (the
board enumerating directly to the PC as a USB ADB device, matching how
real Android devices work), not a UART-based workaround requiring a
PC-side bridge script. UART/TCP-bridge was a reasonable intermediate step
but is not the target architecture, so further work on it was stopped by
explicit decision.

**Reusable artifacts:** `packet.c`/`adb.h` (ADB packet framing) remain
valid and hardware-independent — they will be reused for any future
transport, including USB. `transport.c`/`transport.h` (UART-specific) and
`cnxn.c`/`cnxn.h` (handshake logic written against the UART transport) are
no longer part of the active build target but are kept in version control
for reference.

## Native USB ADB investigation — BLOCKED (hardware limitation, not software)

### Investigation performed (Zephyr 4.4.99 source tree, board DTS)
Checked, on the actual local Zephyr tree — no assumptions made:
1. SoC devicetree (`zephyr/dts/arm/nxp/mcx/mcxw/`) for any USB-related node:
   **none found** — not even a disabled one.
2. Board's compiled DTS (`build/zephyr/zephyr.dts`) for USB nodes:
   **none found**.
3. Zephyr's USB device-controller drivers (`drivers/usb/udc/`,
   `drivers/usb/device/`) for any reference to `mcxw`/`kw3x`/`mcx_w`:
   **no match** — drivers exist for other NXP parts (e.g. `udc_kinetis.c`,
   `usb_dc_kinetis.c`, `udc_mcux_ehci.c`, `udc_mcux_ip3511.c`) but none
   target this SoC family.
4. NXP HAL under `modules/hal/nxp` for any MCXW71 USB module: **none found**.
5. Board docs (`boards/nxp/frdm_mcxw71/doc/index.rst`) only mention USB in
   the context of the onboard debug probe (J10) providing the virtual COM
   port (`/dev/ttyACM0`, which is lpuart1) — a separate MCU-Link/CMSIS-DAP
   chip, not a USB peripheral wired to the MCXW71 itself.

### Conclusion
**MCXW71 does not implement a USB device controller in silicon.** This is
a hardware limitation, not a Zephyr driver gap — there is no register
interface on this chip for a USB device stack to drive, so no amount of
devicetree/Kconfig work can add this capability. MCXW71 is a
wireless-focused SoC (BLE/802.15.4, Cortex-M33); USB was not included in
its peripheral set.

This directly blocks the stated goal (`adb devices`/`adb shell` over
native USB) on this specific board. No implementation was attempted per
"do not guess hardware support" — investigation was exhaustive before
concluding blocked.

### Options going forward (decision needed)
1. **Different board/SoC** with an actual USB device controller (other MCX
   or Kinetis parts with USB-FS/HS peripherals) if native USB ADB is a
   hard requirement.
2. **External USB device controller chip** bridged to MCXW71 over
   SPI/I2C — genuinely native USB from the PC's perspective, but requires
   new hardware and a driver/Kconfig check for whatever part is chosen.
3. Reconsider the transport requirement given this board's actual
   capabilities (e.g. return to a UART-based approach, accepting the
   PC-side bridge requirement that was previously ruled out).

## MAJOR ARCHITECTURE CHANGE: upstream-quality, generic Zephyr USB ADB function

**New long-term goal:** not just "ADB on FRDM-MCXN236," but a genuinely
upstreamable Zephyr USB device class/function for ADB that works on any
Zephyr-supported SoC with a USB device controller. FRDM-MCXN236 is now
explicitly the *first development/test board*, not the final target.

### Investigation: real upstream USB class conventions (Zephyr 4.4.99 tree)
Verified directly against source, not assumed:
1. **File/Kconfig split convention**: `subsys/usb/device_next/class/usbd_<name>.c`
   (impl) + `include/zephyr/usb/class/usbd_<name>.h` (public API) +
   `class/Kconfig.<name>` (sourced via one `rsource` line in `class/Kconfig`).
2. **Kconfig pattern** (from `Kconfig.loopback`): bool enable option +
   `subsys/logging/Kconfig.template.log_config` +
   `subsys/usb/common/Kconfig.template.instances_count`. No VID/PID in any
   class Kconfig, confirmed by grep across all class Kconfig files.
3. **VID/PID is an application-level concern**, confirmed directly in
   `tests/subsys/usb/device_next/src/main.c`:
   `USBD_DEVICE_DEFINE(test_usbd, ..., 0x2fe3, 0xffff)` is called at the
   test/app level, never inside a class.
4. **Hardware independence confirmed** by reading `loopback.c` in full:
   zero board/SoC-specific code anywhere — only `usbd_class_data`/
   `usbd_class_api`, `net_buf`/`udc_buf_info`, FS/HS descriptor variants
   selected via `usbd_bus_speed()`.
5. **Composite device support**: multi-interface functions use
   `USB_DESC_INTERFACE_ASSOC` (IAD); interface numbers start at 0 in each
   class's own descriptor and are renumbered by the core when composed
   with other classes in a configuration. A single-interface function
   (ADB: one interface, two bulk endpoints) doesn't need an IAD.
6. **Hardware-independent test pattern**: `tests/subsys/usb/device_next`
   uses `ztest` on `native_sim` (with a simulated `zephyr_uhc0` host
   controller for full device↔host loop tests, no real hardware). Pure
   protocol logic can be tested even more simply, with no USB simulation
   at all.
7. **License**: every file uses `SPDX-License-Identifier: Apache-2.0`,
   compatible with upstream contribution.

### Proposed architecture: two independently upstreamable layers

**Layer 1 — `usbd_adb` (the actual upstream contribution target):** a raw
USB transport class exposing a real ADB-discoverable interface (class
`0xFF`, subclass `0x42`, protocol `0x01`, one bulk-IN + one bulk-OUT
endpoint, FS+HS descriptor variants per `usbd_bus_speed()`). Knows nothing
about ADB packet framing or protocol semantics — just moves raw bytes,
the same relationship `usbd_cdc_acm` has to whatever's sent over its UART
abstraction.

Proposed file layout (matches real upstream paths exactly):
```
subsys/usb/device_next/class/usbd_adb.c       (hardware-independent class impl)
subsys/usb/device_next/class/Kconfig.adb       (bool + log_config + instances_count)
subsys/usb/device_next/class/Kconfig           (+ rsource "Kconfig.adb")
include/zephyr/usb/class/usbd_adb.h            (public API: rx callback, send)
```
Public API shape (sketch — exact macro conventions to be confirmed against
`usbd_cdc_acm.h` before implementation, not guessed):
```c
int usbd_adb_set_rx_callback(<instance>, rx_cb_t cb, void *ctx);
int usbd_adb_send(<instance>, const uint8_t *data, size_t len);
```

**Layer 2 — ADB protocol engine (stays in our application, NOT proposed
for Zephyr core):** existing `adb.h`/`packet.c` (already hardware- and
transport-independent) plus `cnxn.c` and future OPEN/WRTE/shell-mapping
logic. Depends only on Layer 1's rx-callback/send interface — would work
unchanged over UART, BLE, or any other transport. No Zephyr-core
precedent for an "ADB service" subsystem, so this isn't an upstream
target; it may become a separate Zephyr module later if there's appetite,
but stays app-level for now.

### VID/PID and composite-device handling
VID/PID and `USBD_DEVICE_DEFINE`/configuration/string-descriptor setup
stay entirely in the application (our own `main.c`, replicating
`sample_usbd_init.c`'s pattern with project-specific values) — never
inside `usbd_adb.c`. Because `usbd_adb` is just one more
`USBD_DEFINE_CLASS` instance, it composes with any other class (CDC-ACM
console, MSC, etc.) in the same configuration exactly like
`loopback`/`cdc_acm`/`msc` already do today.

### Test plan
- **Protocol unit tests** (no hardware, no USB simulation): `ztest` suite
  directly exercising `adb_encode`/`adb_decode`/`adb_checksum` — round
  trip, truncated input, oversized payload, bad magic, bad checksum. Runs
  on `native_sim`; modeled on `tests/subsys/usb/device_next`'s
  `prj.conf`/`tests.yaml` shape but with no UDC/UHC config needed.
- **Class-level test** (phase 2 / stretch): `native_sim` + simulated
  `zephyr_uhc0` test verifying `usbd_adb`'s descriptors/endpoint
  enumeration, modeled directly on `tests/subsys/usb/device_next/src/main.c`.
- **Hardware integration**: FRDM-MCXN236 remains the first real-board
  test (`adb devices`/`adb shell` against `udc_mcux_ehci`), run only once
  both test suites above pass.

### Licensing / contribution logistics
All new files will carry `SPDX-License-Identifier: Apache-2.0`, matching
every file inspected in the upstream tree. Since `~/zephyrproject/zephyr`
is a plain clone (not necessarily a personal fork), before writing files
at their real upstream paths we should decide: fork Zephyr on GitHub and
work on a branch there, or develop locally uncommitted and prepare a diff
for a future PR. Not a blocker yet, but a decision needed before
`usbd_adb.c` is actually created at its upstream-path location.

## Immediate next step
No code written yet, per explicit instruction. Awaiting confirmation on:
(1) the exact `usbd_adb.h` public API shape (needs one more inspection
pass against `usbd_cdc_acm.h`'s real macro conventions before finalizing),
(2) the dev-workflow decision (fork+branch vs. local diff), then
implementation of Layer 1 (`usbd_adb` class) begins, followed by
protocol unit tests, then MCXN236 hardware integration.
