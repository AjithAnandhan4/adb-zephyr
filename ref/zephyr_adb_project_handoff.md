# Zephyr ADB USB Feature — Project Handoff / Context

Date: 2026-09-06
Purpose: Full handoff for continuing the ADB-for-Zephyr work on another PC/session.

---

## 1. PROJECT GOAL

Build ADB support for Zephyr so a Linux development PC can interact with a Zephyr device using the normal Android Debug Bridge workflow:

    adb devices
    adb shell
    adb push
    adb pull
    adb reboot

The long-term goal is not merely a demo on one NXP board. The goal is to determine whether a reusable, upstream-quality Zephyr USB ADB function/class makes sense and, if so, develop it in a form that could eventually be proposed upstream.

Important: upstream acceptance is NOT guaranteed. We should optimize for clean architecture, existing Zephyr conventions, portability, tests, and a focused contribution.

Preferred architecture:

    Linux adb
        |
        | USB
        v
    Zephyr USB ADB function/class
        |
        | raw byte transport
        v
    ADB protocol engine
        |
        +--> Zephyr shell
        +--> filesystem/sync
        +--> reboot/state
        +--> MCUboot/update (later)

The USB class should NOT know about shell semantics, CNXN/OPEN/WRTE semantics, filesystem operations, or MCUboot.

---

## 2. CURRENT DEVELOPMENT TARGET

Primary hardware target:

    FRDM-MCXN236

Why:

- MCXN236 has USB HS host/device capability and an integrated transceiver.
- The FRDM-MCXN236 USB Type-C connector is connected to the target MCU USB interface.
- NXP provides USB device/CDC examples for this platform.
- Zephyr has board support for frdm_mcxn236.

The original board was:

    FRDM-MCXW71

MCXW71 was abandoned for the production USB path because investigation showed no suitable native USB device controller exposed by the board/Zephyr architecture. The board USB-C connector is associated with MCU-Link/debug functionality rather than being a target-MCU USB device port.

Do NOT return to the UART-based production architecture.

The MCXW71 UART work is still valuable as a protocol/peripheral prototype and proof that the ADB packet layer works.

---

## 3. WHAT HAS ALREADY BEEN PROVEN

### 3.1 ADB packet layer

The ADB packet framing implementation works.

Files used in the prototype:

    include/adb.h
    src/packet.c

Header:

    command
    arg0
    arg1
    data_length
    data_checksum
    magic

ADB header is 24 bytes.

Constants:

    ADB_MAX_PAYLOAD    4096
    ADB_HEADER_SIZE    24

Commands already defined:

    CNXN = 0x4E584E43
    AUTH = 0x48545541
    OPEN = 0x4E45504F
    OKAY = 0x59414B4F
    CLSE = 0x45534C43
    WRTE = 0x45545257

ADB version:

    0x01000000

Packet checksum is the byte sum of the payload.

Magic is:

    command ^ 0xFFFFFFFF

The packet encode/decode test passed.

Observed test:

    *** Booting Zephyr OS build v4.4.0-14283-g62acbd571c72 ***
    === ADB packet layer test ===
    Encoded CNXN packet: 40 bytes
    Decoded header: command=0x4e584e43 arg0=0x01000000 arg1=0x00001000 length=16 checksum=0x00000628 magic=0xb1a7b1bc
    PASS: CNXN packet round-trip OK
    Payload (16 bytes): "host::adb-zephyr"
    adb packet test idle

Important debugging discovery:
A local stack allocation like:

    uint8_t encoded[24 + 4096];

in Zephyr main caused apparent failure/no prints because of the small main stack. Static/global buffers fixed this.

Do not accidentally reintroduce large stack buffers.

### 3.2 UART transport prototype

A UART transport was implemented temporarily on MCXW71 using LPUART0.

The transport used:

- interrupt RX
- ring buffer
- semaphore
- static RX/TX/parse storage

Zephyr 4.4 detail:

    uart_irq_update(dev);

returns void, so do NOT write code expecting it to return a value.

The transport loopback worked with a physical jumper:

    LPUART0 TX PTA17 -> LPUART0 RX PTA16

Observed:

    *** Booting Zephyr OS build v4.4.0-14283-g62acbd571c72 ***
    === ADB UART transport loopback test ===
    Requires jumper: PTA17 (LPUART0_TX) -> PTA16 (LPUART0_RX)
    Sent CNXN packet (16 byte payload) over lpuart0
    PASS: transport loopback round-trip OK
    Received payload: "host::adb-zephyr"
    transport test idle

This proves packet encoding/decoding plus actual UART peripheral/pins/interrupt path.

But raw UART is NOT the desired final ADB interface because stock Linux `adb` expects the ADB USB transport (or another supported transport), not an arbitrary raw serial framing. A PC-side serial-to-ADB bridge would be required, which defeats the desired normal office workflow.

---

## 4. EXISTING ZEPHYR USB ARCHITECTURE INSPECTED

The Zephyr tree was inspected around:

    subsys/usb/device_next/class

Existing USB functions include examples such as:

    usbd_cdc_acm.c
    usbd_msc.c
    usbd_hid.c
    usbd_uac2.c
    usbd_uvc.c
    usbd_dfu.c
    bt_hci.c
    loopback.c

The closest structural analog identified is:

    loopback.c

because it exposes a vendor-specific interface with raw bulk endpoints and does not impose application protocol semantics.

Zephyr conventions identified:

### Implementation

    subsys/usb/device_next/class/usbd_<name>.c

### Public API

    include/zephyr/usb/class/usbd_<name>.h

### Kconfig

    subsys/usb/device_next/class/Kconfig.<name>

and include it from class/Kconfig using:

    rsource "Kconfig.<name>"

### VID/PID

VID/PID belongs to the application/device configuration, not the reusable class.

For example, Zephyr tests can define a USB device with their own VID/PID.

Therefore:

    usbd_adb.c

MUST NOT hard-code a VID/PID.

### Hardware independence

The class implementation should not contain MCXN236 register manipulation or board-specific code.

It should use Zephyr's public USB device_next APIs.

USB controller/backend details remain below the class layer.

### Composite USB

ADB should be designed so it can coexist with other USB functions such as CDC ACM, MSC, etc.

A single-interface ADB function does not need an IAD. IAD becomes relevant for multi-interface functions.

### License

New upstream-targeted files should use Apache-2.0 licensing consistent with Zephyr.

---

## 5. PROPOSED UPSTREAM ARCHITECTURE

The likely upstream contribution is:

    usbd_adb

as a generic USB function/class.

Proposed files:

    zephyr/
      subsys/usb/device_next/class/usbd_adb.c
      subsys/usb/device_next/class/Kconfig.adb
      include/zephyr/usb/class/usbd_adb.h

and:

    subsys/usb/device_next/class/Kconfig

gets:

    rsource "Kconfig.adb"

The exact public API is NOT finalized yet.

Initial concept was something like:

    int usbd_adb_set_rx_callback(
        <instance>,
        void (*cb)(const uint8_t *data, size_t len, void *ctx),
        void *ctx);

    int usbd_adb_send(
        <instance>,
        const uint8_t *data,
        size_t len);

BUT this must be compared against current Zephyr USB class API conventions before locking it.

Do not blindly implement this exact API if existing Zephyr patterns suggest a better interface.

---

## 6. ADB USB INTERFACE REQUIREMENTS

The ADB USB function needs to expose the interface that the host ADB implementation expects.

The working architectural target is:

    class      0xFF
    subclass   0x42
    protocol   0x01

with:

    one bulk OUT endpoint
    one bulk IN endpoint

The class should support the appropriate FS/HS descriptor variants based on:

    usbd_bus_speed()

Do not assume USB enumeration alone proves ADB compatibility.

The actual acceptance test is eventually:

    Linux adb
       |
       | USB
       v
    FRDM-MCXN236
       |
       +--> adb devices
       +--> adb shell

Descriptors, endpoint direction, packet sizes, interface identity, and actual host behavior must be validated with a real Linux adb client.

---

## 7. IMPORTANT ADB PROTOCOL CONTEXT

ADB packet header:

    command       uint32
    arg0          uint32
    arg1          uint32
    data_length   uint32
    data_checksum uint32
    magic         uint32

All fields are little-endian.

Typical commands include:

    CNXN
    AUTH
    OPEN
    OKAY
    CLSE
    WRTE

Current ADB implementations may also use:

    STLS

for transport-layer security negotiation.

Do not assume the minimal old protocol is sufficient for every modern adb client.

The first useful milestone is still:

    USB enumeration
        ->
    host sees an ADB-compatible USB interface
        ->
    adb devices
        ->
    CNXN exchange
        ->
    online device

Then:

    adb shell

Then:

    adb push/pull

Then:

    adb reboot

Then later:

    authentication/security
    MCUboot integration
    additional features

---

## 8. ADB PROTOCOL ENGINE BOUNDARY

The protocol engine should remain separate from the USB class.

Current application/prototype files:

    include/adb.h
    src/packet.c

Future likely protocol modules:

    cnxn.c
    auth.c
    stream.c
    shell.c
    sync.c
    reboot.c

The USB layer should only provide a byte pipe.

Conceptually:

    usbd_adb
        RX bytes ---> ADB protocol engine
        TX bytes <--- ADB protocol engine

The protocol engine should not know whether its transport is USB, a test transport, or something else.

This separation makes unit testing much easier.

---

## 9. DEVELOPMENT STRUCTURE

Recommended workspace:

    ~/zephyrproject/
    ├── zephyr/
    │   └── upstream Zephyr clone / feature branch
    │
    └── adb-zephyr/
        └── integration/demo application

The generic USB class should be developed in the Zephyr tree.

The demo/application should remain separate.

Recommended Zephyr branch:

    cd ~/zephyrproject/zephyr
    git checkout -b feat/usbd-adb

The project should remain easy to turn into a clean upstream PR.

Do NOT create a second unrelated application architecture just because the previous Claude session expired.

---

## 10. FORK VS DOWNSTREAM VS UPSTREAM-FIRST DECISION

This is intentionally NOT locked.

We should decide based on what makes technical and upstream workflow sense.

Preferred process:

1. Develop in a clean feature branch in the local Zephyr clone.
2. Keep the implementation generic.
3. Build and test on MCXN236.
4. Add unit/integration tests.
5. Review against current Zephyr APIs and contribution conventions.
6. If the feature clearly belongs in Zephyr core, prepare an upstream-quality patch/PR.
7. If upstream maintainers would reject the scope or API, keep the generic implementation downstream or as a separate module and document why.

A fork of Zephyr is useful if the workflow requires pushing the branch to a remote for collaboration/PR preparation, but a GitHub/GitLab fork should not be treated as a reason to fork the architecture.

The important distinction is:

    Git fork = collaboration/version-control mechanism

versus:

    downstream implementation = product/project code that does not need upstream acceptance

The architecture should be upstream-quality first, while the final repository location can be decided later.

Do not over-engineer the feature solely to make it "upstreamable."

---

## 11. TEST STRATEGY

### Protocol unit tests

Use Zephyr ztest, preferably on native_sim where practical.

Test:

- encode/decode round trip
- zero-length packet
- maximum payload
- truncated header
- truncated payload
- oversized payload
- invalid magic
- invalid checksum
- malformed fields

### USB class tests

Potentially:

- descriptor correctness
- endpoint configuration
- USB enumeration
- RX/TX byte movement

Zephyr's USB device_next tests and simulated UDC/UHC infrastructure should be inspected before inventing a custom test framework.

### Hardware integration

Primary:

    FRDM-MCXN236

Host:

    Linux

Real acceptance tests:

    adb devices
    adb shell
    adb push
    adb pull
    adb reboot

The first hardware milestone is USB enumeration and ADB discovery.

---

## 12. CURRENT SOFTWARE ENVIRONMENT FROM THE PREVIOUS MACHINE

Workspace:

    ~/zephyrproject

Zephyr:

    /home/ajianan/zephyrproject/zephyr

Zephyr version/build observed:

    4.4.99
    v4.4.0-14283-g62acbd571c72

Zephyr SDK:

    1.0.1

west:

    1.5.0

CMake:

    4.0.2

Python:

    3.12.13

Original application:

    ~/zephyrproject/adb-zephyr

Original board:

    frdm_mcxw71
    qualifier: mcxw716c

New board:

    frdm_mcxn236

The new PC may have different versions. Always inspect the actual installed Zephyr tree before assuming APIs.

---

## 13. IMMEDIATE NEXT TASK

The previous Claude session had completed architecture/source inspection but had NOT yet implemented the USB ADB class.

Therefore the next task is:

    Implement generic usbd_adb USB class skeleton/function
    in the current Zephyr tree.

Before coding:

1. Read this handoff.
2. Read ADB_PROGRESS.md if it exists.
3. Inspect the actual current Zephyr source tree.
4. Compare:
       usbd_cdc_acm
       loopback
       other device_next classes
5. Determine the correct current APIs.
6. Implement the smallest clean generic ADB USB function.
7. Add Kconfig.
8. Add public header/API.
9. Build on frdm_mcxn236.
10. Fix compile/API issues based on the actual tree.
11. Do not start implementing shell/sync yet.
12. Report files changed, design decisions, build result, and remaining blockers.

First milestone:

    Generic usbd_adb class compiles
        ->
    FRDM-MCXN236 USB enumerates
        ->
    descriptor/interface verified
        ->
    raw bulk RX/TX works

Second milestone:

    Linux adb recognizes device
        ->
    CNXN exchange works
        ->
    adb devices shows device

Third milestone:

    adb shell

Only after those work should we move to sync/push/pull/reboot.

---

## 14. WHAT NOT TO DO

Do NOT:

- return to raw UART as the final transport
- create an MCXN236-only ADB USB implementation
- hard-code VID/PID into the reusable class
- put ADB protocol semantics inside the USB class
- put board-specific register code into usbd_adb.c
- assume USB enumeration means adb compatibility
- allocate multi-kilobyte packet buffers on the Zephyr main stack
- start with shell/filesystem/MCUboot before the USB transport works
- redesign the whole project because a Claude session expired
- blindly copy APIs from an older Zephyr version
- assume STLS/auth requirements can be ignored indefinitely

---

## 15. ROLE OF CLAUDE CODE SUB / THIS HANDOFF

The development workflow can use Claude Code on another PC for large coding passes.

Use this document as the persistent project context.

Claude should be allowed to make implementation-level decisions after inspecting the current Zephyr tree.

A good division of responsibility:

    Claude:
        inspect source tree
        implement code
        build
        fix compiler/API issues
        add tests
        maintain ADB_PROGRESS.md

    Technical review:
        verify architecture
        catch accidental board-specific design
        review USB descriptors
        review API boundaries
        challenge assumptions
        verify upstream suitability
        help debug host/device interoperability

If Claude proposes a significant architecture change, explain why before proceeding.

---

## 16. SUCCESS CRITERIA

The project is successful when a normal Linux development workflow can use:

    $ adb devices

and see the Zephyr target.

Then:

    $ adb shell

should provide a useful Zephyr shell/session.

Later:

    $ adb push file ...
    $ adb pull ...
    $ adb reboot

should map cleanly onto Zephyr capabilities.

For upstream success, the reusable USB class should be:

- generic
- hardware independent
- based on Zephyr device_next APIs
- composable with other USB functions
- tested
- documented
- free of product-specific VID/PID assumptions
- free of MCXN236-specific code
- separated cleanly from the ADB protocol engine

---

## 17. SHORT EXECUTIVE SUMMARY

We are building ADB support for Zephyr.

MCXW71 was tested with an ADB packet layer and UART transport, but UART is not the desired production interface and MCXW71 is not suitable for the native USB device path.

We moved to FRDM-MCXN236 because it provides USB device capability.

The key architectural decision is:

    Generic Zephyr USB function: usbd_adb
            |
            | raw bytes
            v
    Separate ADB protocol engine
            |
            v
    Zephyr shell/filesystem/etc.

The reusable USB class should be developed in the Zephyr tree with upstream-quality conventions. The application remains separate.

The next concrete job is implementing and testing `usbd_adb` itself.

Do not restart from scratch. Inspect the current Zephyr tree and continue from this point.
