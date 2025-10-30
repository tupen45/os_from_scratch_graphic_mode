USB tethering design and implementation plan

Goal: implement USB tethering support so the OS can use a connected smartphone or host as a network interface providing real Internet access on real hardware.

High-level approach

1) Research and assumptions
   - This kernel is a minimal 32-bit hobby OS with PCI Ethernet drivers and a simple net stack (`net.c`).
   - No existing USB host controller driver present; we must implement a USB host stack or a lightweight driver targeting common host controllers (e.g., xHCI) or rely on USB device “gadget” is not applicable since we are running as a host OS on PC hardware.
   - For tethering to work with real phones, the OS should implement a USB CDC-ECM/RNDIS client or use CDC-ECM/ECM-over-USB or RNDIS protocols. Many Android phones use RNDIS; iPhone uses CDC-ECM/PPP historically.

2) Recommended minimal path (practical for this repo):
   - Implement a basic USB host stack limited to xHCI (modern PCs) or EHCI/OHCI if xHCI is complex. xHCI covers modern hardware but is larger. EHCI+OHCI are older.
   - Alternatively, implement USB-over-Ethernet user-space glue is impossible here: we need kernel USB host to access device descriptors and endpoints.
   - Implement USB host controller driver for xHCI (core MMIO access, context/command ring, event ring) — non-trivial.

3) Pragmatic phased plan (step-by-step)
   Phase A - discovery and scaffolding (this change)
     - Add `usb/` directory with notes, headers, and small skeletons.
     - Add a `usb_stub` NIC backend that implements `nic_tx`/rx hooks to route network packets to a userdriver for testing.
     - Add build integration points in `Makefile.inc` once initial code compiles.

   Phase B - simple USB host support
     - Implement PCI probe for xHCI (vendor/device class 0x0C0330) and map BAR.
     - Implement minimal xHCI initialization to enumerate devices (only control transfers and a few descriptors). This is large.

   Phase C - RNDIS/CDC-ECM client
     - After control/basic transfer support, detect tethering interfaces, bind an RNDIS or CDC-ECM driver, claim interface, and set up bulk endpoints for data exchange.
     - Translate between USB bulk frames and `net_rx`/`nic_tx`.

   Phase D - polishing
     - DHCP client, ARP, MTU handling, and proper concurrency/interrupts.

4) Test strategy
   - Start with a USB test harness in QEMU (usb device emulation) and a simple virtual RNDIS device.
   - On real hardware, test with an Android phone supporting USB tethering (RNDIS). Device must be connected and allowed to tether.

5) Risks and time estimates
   - Implementing xHCI is moderately complex (~1-3 weeks of focused work depending on prior experience).
   - Alternative: use a small USB host stack library; but adding external dependencies to this kernel is non-trivial.

Next steps (I'll implement Phase A now):
 - Create `usb/usb.h` and `usb/usb_host.c` skeletons.
 - Add `usb/usb_console.c` to exercise USB enumeration via a stub.
 - Add a `usb/nic_stub.c` that exposes a `nic_tx`-compatible interface so your existing network stack can be tested before a real host controller exists.

Notes: I will assume kernel provides `kmalloc`, port/IO/MMIO helpers and PCI helpers (already present). If you'd prefer the quicker but narrower approach (e.g., support only RNDIS by implementing usb over UHCI/EHCI), tell me and I can pivot.
