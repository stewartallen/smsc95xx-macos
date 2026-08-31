# Building and loading SMSC95xxDriver.dext

> **Scope as of M4.** This driver registers a network interface and reads the chip's registers.
> It does **not** move frames yet — there is no USB bulk transfer path until M5. `ifconfig` will show
> the interface with the correct MAC and `status: active`, but nothing can be sent or received and
> `ping` will not work. Do not read `status: active` as reachability; see the link-state section for
> why it reports active whenever the dongle is attached.


The DriverKit extension (`dext`) for the LAN9500A is built entirely with the `Makefile` — no Xcode
project. Every non-obvious flag has been validated on hardware and is documented here.

---

## Prerequisites

This driver requires **SIP disabled** on macOS. SIP intercepts system extension loading at the kernel
level and cannot be bypassed from userspace. **This is not production-ready; it is a development
posture.**

1. **Boot into Recovery** — restart holding the power button, select Options, then Utilities, then Terminal.

2. **Disable SIP:**
   ```sh
   csrutil disable
   reboot
   ```

3. **Install the AMFI boot-arg** — required because the maintainer's Apple Developer account is
   pending activation, so no provisioning profile can yet authorise the restricted
   `com.apple.developer.system-extension.install` entitlement. Without this boot-arg the installer
   app is SIGKILLed at exec with exit code 137. Once a real Apple Development identity with proper
   entitlements becomes available, this step should be skipped and developer mode (see `make install`)
   should handle it via the profile.
   ```sh
   sudo nvram boot-args="amfi_get_out_of_my_way=0x1"
   ```
   **What this does:** disables code-signing entitlement validation **machine-wide**, not just for
   this driver. It is a security regression and should be reverted:
   ```sh
   sudo nvram -d boot-args
   ```
   when the Developer account activates and a real signing identity is available.

4. **Verify the prerequisites:**
   ```sh
   csrutil status                    # should report "disabled"
   nvram boot-args                   # should output "amfi_get_out_of_my_way=0x1"
   systemextensionsctl developer on  # run as root; output will confirm when complete
   ```

---

## Building and installing

All commands run from this directory (`dext/`). Every target re-builds prerequisites automatically.

### `make` — build the dext and installer app

```sh
cd dext
make
```

Builds `build/com.github.stewartallen.smsc95xx.driver.dext` and
`build/SMSC95xxInstaller.app`, both ad-hoc signed. The build system:
- Runs `iig` on the C++ interface definition to generate the vtable glue
- Compiles all code with `arm64e` (pointer authentication, required on Apple Silicon)
- Links against `DriverKit` and `USBDriverKit` frameworks
- Signs both bundles with ad-hoc signatures and full entitlements

**Build artifacts are placed in `build/`, not in the source tree or Xcode default locations.**

### `make install` — stage and activate the dext

```sh
sudo make install
```

Copies the installer app to `/Applications` and runs it to activate the dext. On completion:
- `systemextensionsctl list` will show the dext as enabled and active
- `ioreg` will show a matching `IOUserService` under the device node
- Kernel logs will show matching and `Start()` messages

This requires `sudo` because system extensions can only be activated from an app in
`/Applications`, which requires elevated privilege to write.

### `make status` — check if the dext is loaded and matched

```sh
make status
```

Queries `systemextensionsctl list` for the dext, then searches `ioreg` for a matched
`IOUserService` node. Output:
- If the dext is not installed, `systemextensionsctl` shows nothing
- If installed but not activated, state shows `enabled` but not `activated`
- If activated but not matched, no `IOUserService` appears in `ioreg`
- If matched, the service appears under the device node with the driver name

An inactive/unmatched dext often indicates a matching rule mismatch, a renamed bundle, or
missing entitlements. Check kernel logs first (see Debugging, below).

### `make uninstall` — deactivate and remove the dext

```sh
sudo make uninstall
```

Deactivates the dext and removes the installer app from `/Applications`. Kernel may take a few
seconds to fully tear down the driver after deactivation.

### `make clean` — remove all build artifacts

```sh
make clean
```

Removes the entire `build/` directory.

---

## The dext bundle structure

A DriverKit extension bundle is **flat** — `Info.plist` and the executable sit at the top level
with no `Contents/MacOS` subdirectory. This is different from regular macOS apps.

```
com.github.stewartallen.smsc95xx.driver.dext/
├── Info.plist
├── com.github.stewartallen.smsc95xx.driver   (the executable)
└── [signed]
```

macOS validates this structure: `codesign -d -vvv` will report the bundle's identity correctly,
and `systemextensionsctl list` will discover it. Apple's own dexts in `/System/Library/DriverExtensions`
follow the same structure.

**Critical:** The bundle must be named `<CFBundleIdentifier>.dext` exactly. A bundle named
`SMSC95xxDriver.dext` fails activation with "Unable to find any matched extension with identifier…"
even though the identifier inside `Info.plist` is correct. The Makefile enforces this naming via
`DEXT_BUNDLE := $(BUILD)/$(DEXT_ID).dext` where `DEXT_ID` is the identifier from the plist.

**Also required: `OSBundleUsageDescription`.** Extensions in the
`com.apple.system_extension.driver_extension` category must declare it, or activation fails with
`OSSystemExtensionErrorDomain` code 9 — which does at least say so outright, unlike the other three
failures. Ours reads:

```xml
<key>OSBundleUsageDescription</key>
<string>Drives Microchip/SMSC LAN9500A USB 2.0 Ethernet adapters, including 10BASE-T1S dongles.</string>
```

Note that Apple's preinstalled dexts in `/System/Library/DriverExtensions` do **not** carry this key —
they are not installed through `OSSystemExtensionRequest`, so they never face the check. Copying their
`Info.plist` as a template is therefore not sufficient.

---

## Build system implementation details

### Why `DEVELOPER_DIR` is set in the Makefile

```make
DEVELOPER_DIR := /Applications/Xcode.app/Contents/Developer
export DEVELOPER_DIR
```

The `xcrun` tool uses `DEVELOPER_DIR` to locate the DriverKit SDK. If not set, `xcrun` searches
`$PATH` and falls back to the currently active Xcode version (set by `xcode-select`). This
makefile hardcodes the path for reproducibility — the same build runs identically whether
Xcode is the active version or not. If Xcode is installed elsewhere, override this at build time:

```sh
make DEVELOPER_DIR=/path/to/Xcode.app/Contents/Developer
```

### Why `-D__IIG=1` is required

```make
IIG_FLAGS := -D__IIG=1 -x c++ -std=gnu++17 -isysroot $(DK_SDK)
```

The DriverKit SDK headers (`OSObject.h` and related) are dual-purpose: they serve both the
build-time `iig` code generator and the C++ compiler. The `__IIG` define gates code that only
`iig` should see (macros that expand to vtable scaffolding). Without `-D__IIG=1`, every `.iig`
header fails to parse in `iig` itself, producing "KERNEL/LOCALONLY scope illegal" errors.

### Why `arm64e` and not `arm64`

```make
DK_TARGET := arm64e-apple-driverkit24.0
```

Pointer authentication (`arm64e`) is mandatory on Apple Silicon. A plain `arm64` build builds,
links, signs, stages, and activates without complaint, then fails at runtime with:
```
kernelmanagerd: "DextLaunch(...)": NSPOSIXErrorDomain Code=8 "Exec format error"
```

Nothing in the IOKit personality is wrong; the Mach-O simply will not execute. Apple's own dexts
in `/System/Library/DriverExtensions` are all `arm64e`. This is an architecture requirement, not
a configuration mistake.

### Where the entitlements came from

The `dext/SMSC95xxDriver/SMSC95xxDriver.entitlements` file specifies:
The dext (`SMSC95xxDriver/SMSC95xxDriver.entitlements`) carries three:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.transport.usb`
- `com.apple.developer.driverkit.family.networking` — claimed ahead of need, so an entitlement
  rejection surfaces now rather than at M4 alongside a new queue model

The installer app (`SMSC95xxInstaller/Installer.entitlements`) carries one:

- `com.apple.developer.system-extension.install`

These values were measured from Apple's own dexts on this machine:
- `com.apple.DriverKit-AppleUSBFTDI.dext` (signed by Apple, in `/System/Library/DriverExtensions`)
- `com.apple.DriverKit.AppleUserECM.dext` (signed by Apple, in `/System/Library/DriverExtensions`)

Extracted with:
```sh
codesign -d -vvv --entitlements - /System/Library/DriverExtensions/*.dext
```

`com.apple.developer.system-extension.install` is a **restricted** entitlement: authorising it needs a
provisioning profile, which needs an active Apple Developer account. Under an unauthorised (here,
ad-hoc) signature **AMFI** SIGKILLs the process at exec — the installer exits 137 before `main` runs.

The gate is AMFI, not SIP: this was measured with SIP already disabled, and the same binary in the same
directory exits 2 and prints usage once re-signed *without* the entitlement. That is why
`amfi_get_out_of_my_way=0x1` is in the Prerequisites, and why it can be dropped as soon as a real
signing identity is available.

---

## Device matching and the provider

### Why the provider is `IOUSBHostDevice`, not `IOUSBHostInterface`

The device arrives at the macOS USB stack **unconfigured** — with configuration 0 selected. Before
the driver can access endpoints, it must select a configuration and copy an interface node. See
`reference/m3-attach-state.txt` for the measured state at clean attach.

Unconfigured devices have no interface nodes in the IOKit tree — only a device node. A personality
matching `IOUSBHostInterface` can never match, because no interface exists. The dext builds, signs,
installs, and activates cleanly, then silently never matches, with nothing logged to say why.

To check whether the driver actually attached, search the USB device subtree — **not**
`ioreg -c IOUserService`, whose class filter matches nothing for this driver even when it *is* attached
and so reads as a false negative:

```sh
ioreg -w0 -r -c IOUSBHostDevice -l | grep -E 'SMSC95xxDriver|"SMSC95xx[A-Za-z]+"'
```

`make status` runs exactly this. Do not match on the device's product name either: it is absent when the
EEPROM auto-load fails.

The solution: match the device node directly with `IOUSBHostDevice` as the provider, then select
configuration 1 in `Start()` and copy interface 0. The kernel independently confirms this in the
logs:
```
IOUSBHostFamily: IOUSBHostDevice::setConfigurationGated: SMSC95xxDriver selected configuration 1
```

### Personalities must use device-level match keys

A personality whose `IOProviderClass` is `IOUSBHostDevice` must **not** require
`bConfigurationValue` or `bInterfaceNumber`. Those are properties of `IOUSBHostInterface` registry
nodes; a device node has neither, so a dictionary demanding them can never match — and nothing
reports an error, the driver simply never attaches. Verified against the live node's full property
list, which carries `idVendor`, `idProduct`, `bDeviceClass`, `bDeviceSubClass`, `bDeviceProtocol`,
`bcdDevice` and `bNumConfigurations`, and no per-interface keys at all.

This is an easy trap when converting an interface personality to a device one, because Apple's
USB dext examples (`AppleUSBFTDI`, `AppleUserECM`) all match interfaces and so legitimately use those
keys — in both their personalities *and* their `transport.usb` entitlement.

### `SetConfiguration` and the `matchInterfaces` parameter

The DriverKit USB API differs from the userspace IOUSBLib API that `smsc95xx-probe` uses, in three
ways that all cost time if assumed:

- **No `GetConfiguration()`.** IOUSBLib has one; `IOUSBHostDevice` does not. To find out whether the
  device is configured, probe for a child interface — they exist only once a configuration is
  selected. (`CopyConfigurationDescriptor(IOService *)` is the other option.)
- **`CopyInterface(uintptr_t ref, IOUSBHostInterface **)` is an iterator call, not an index.** Pair it
  with `CreateInterfaceIterator` / `DestroyInterfaceIterator`. Passing `0` as the ref is wrong.
- **`Open` argument types differ between the two classes.** `IOUSBHostDevice::Open` takes
  `uintptr_t`; `IOUSBHostInterface::Open` takes `uint8_t *`. They are not interchangeable.

The real signature is two arguments:

```cpp
kern_return_t SetConfiguration(uint8_t bConfigurationValue, bool matchInterfaces);
```

The driver calls `SetConfiguration(1, false)`, and only after finding no interface. Both details
matter:

- **Probe first, do not configure unconditionally.** The SDK is explicit that "if the device was
  previously configured all child interfaces will be terminated", so re-issuing SET_CONFIGURATION on
  an already-configured device would tear down interfaces another driver may hold. `Start()` therefore
  looks for an interface first and configures only when none is found.
- **`matchInterfaces = false`** keeps the interface from being published for other drivers to match
  while this driver owns the device. `false` proved sufficient; `true` was never needed.

---

## Networking personality change in M4

The transition from M3 (dext loads and matches) to M4 (interface appears) required a change in the
IOKit personality and bundle configuration to shift from generic `IOUserService` to network-specific
`IOUserNetworkEthernet`. This is not discoverable from Apple's documentation but is essential:

**In `Info.plist` personalities:**
- `IOClass` changed from `IOUserService` to `IOUserNetworkEthernet`
- `CFBundleIdentifierKernel` changed from `com.apple.kpi.iokit` to `com.apple.iokit.IOSkywalkFamily`

**In the bundle root:**
- Added `OSBundleLibraries` with `com.apple.iokit.IOSkywalkFamily = 1.0`

**`IOProviderClass` stays `IOUSBHostDevice`** for the reason already documented above: the device
arrives unconfigured, with no interface nodes in the IOKit tree. Device-level matching is the only
option.

### Five NetworkingDriverKit facts that cost real time

Each of these cost hours. In fairness, three of them *are* readable in the SDK headers if you know
where to look — the `__deprecated … = 0` markers, `registerEthernetInterface`'s doxygen block, and the
`QUEUENAME` attributes are all right there. The problem is that none is in Apple's prose documentation,
and none announces itself as load-bearing until something silently fails. They are recorded here with
their symptoms so the symptom leads back to the cause:

**Capital/lowercase method pairs.** `IOUserNetworkEthernet` declares several methods twice: a
deprecated capital-initial form that is pure virtual and must be overridden for the class to
instantiate, and a live lowercase form that the stack actually calls. Overriding only the deprecated
one compiles, loads, and silently does nothing. Affects:
- `SetInterfaceEnable` / `setInterfaceEnable`
- `GetHardwareAssists` / `getHardwareAssists`
- `ReportLinkStatus` / `reportLinkStatus`
- `RegisterEthernetInterface` / `registerEthernetInterface`

**Nine methods are both `__deprecated` and pure virtual.** All must be stubbed or the class will not
instantiate; a missing one presents as the dext server launching and immediately exiting, with nothing
logged to the kernel. The nine are: `SetInterfaceEnable`, `SetMulticastAddresses`,
`SetAllMulticastModeEnable`, `SelectMediaType`, `SetWakeOnMagicPacketEnable`, `SetMTU`,
`GetMaxTransferUnit`, `SetHardwareAssists`, `GetHardwareAssists`.

**`registerEthernetInterface` (NDK_22, lowercase) takes no MAC.** The real signature, from
`IOUserNetworkEthernet.iig`:

```cpp
IOReturn registerEthernetInterface(IOUserNetworkPacketQueue **queues, uint32_t numQueues,
                                   IOUserNetworkPacketBufferPool *txPool,
                                   IOUserNetworkPacketBufferPool *rxPool) LOCALONLY NDK_22;
```

A single array of `IOUserNetworkPacketQueue *` — the four concrete queue classes all derive from it —
and no MAC parameter. The MAC arrives via a `getHardwareAddress(ether_addr_t *)` override instead. One
pool may serve as both `txPool` and `rxPool`; the header's own documentation says so explicitly.

**Link status must be reported from `setInterfaceEnable(true)`, not only from `Start()`.**
Reported only from `Start()`, the value is discarded before the interface is enabled and `ifconfig`
shows `media: autoselect (none)` with no `status:` line. Reporting it again from
`setInterfaceEnable(true)` — the live lowercase callback the stack invokes on `ifconfig up` —
produces the correct output. Both calls are kept.

**`TxDispatchQueue` / `RxDispatchQueue` are NOT needed in `Info.plist`.** Those QUEUENAME attributes
sit on framework-internal LOCAL methods a driver never implements. The four queue `Create` calls each
take an `IODispatchQueue *` the driver supplies, and no Apple-shipped dext declares these keys.

---

## Link-state policy

The interface reports `status: active` whenever the dongle is attached, whether or not the cable is
plugged in. This is defensible on a PLCA-less 10BASE-T1S multidrop segment: there is no continuous
link-integrity signal to read while the medium is quiet, so the honest model is to report the link
up whenever the device is present.

**Important:** `status: active` does **not** mean the cable is plugged in. It does not track cable
state. On this hardware, `BMSR` bit 2 (link status) reads set even with the 10BASE-T1S cable physically
unplugged, measured twelve consecutive times with the Linux driver out of the path. Under IEEE clause 22
the link-status bit is latching-low, so a PHY that had detected the failure would show it clear on the
first read. This PHY does not.

Real T1S link and PLCA state live in IEEE clause-45 MMD registers, reachable through the clause-22
indirect registers 13/14 that the driver's existing `miiRead`/`miiWrite` transport already supports.
This is deferred, not blocked.

See `reference/m4-interface.txt` section 2 for the measured evidence.

---

## Advertised offloads — open item

The interface advertises `TSO4,TSO6,PARTIAL_CSUM` — TCP segmentation offload and partial checksum
offload — that the driver does not implement. `Start()` never touches `COE_CR` at all — it skips
checksum offload initialization entirely, and there is no packet segmentation logic anywhere in the
driver.

Three driver-side APIs were tried, each returning nothing:
- `getHardwareAssists()` — called repeatedly, returns 0, flags unchanged
- `setHardwareAssists(assists, mask)` — never called by the stack
- `getTSOOptions(opts)` — never called by the stack

**Search for the right symbol.** `kIOEthernetHardwareAssist*` is the in-kernel IONetworkingFamily
prefix and finds nothing under DriverKit — searching for it produces a false conclusion that no such
constants exist. The DriverKit spelling is `kIOUserNetworkHWAssist*`, in
`NetworkingDriverKit/IOUserNetworkTypes.h`:

```c
kIOUserNetworkHWAssistTxChecksumIPHdr = 0x00000001,  TxChecksumTCP = 0x00000002,
kIOUserNetworkHWAssistTxChecksumUDP   = 0x00000004,  TSO4          = 0x00200000,
kIOUserNetworkHWAssistTSO6            = 0x00400000,  RxChecksum    = 0x20000000,
```

Those are exactly the capabilities `ifconfig` names. A fourth API, `getFeatureFlags()` (NDK_22), was
also checked — but the `kIOUserNetworkFeatureFlag*` enum carries only VLAN, timestamp, WOMP and
NicProxy bits, no TSO or checksum, so it cannot be the source either.

So the bounded conclusion is narrower than "no API exists": the driver reports 0 through the one API
the family actually calls, the descriptor the family builds has a `hwAssists` field fed from it, and the
advertisement persists regardless. The remaining hypothesis — untested — is that `options=` reflects
what the **Skywalk layer** offers to the BSD stack, with the offload performed in software above the
driver, rather than a claim about our hardware. `CHANNEL_IO` and `ZEROINVERT_CSUM` appearing in the same
list are consistent with that reading, both being Skywalk-native flags.

**M5 test:** once bulk transfer is wired, check whether the stack hands us packets that are
pre-segmented or carry partial checksums. If it does, transmission will fail in a way that looks
exactly like a framing bug, so check this **before** suspecting the framing code. If the stack
honours the 0 from `getHardwareAssists()` in practice, the advertisement is cosmetic and this item
can be closed.

---

## Important debugging notes

These are not obvious and they cost hours:

### `log` is a zsh builtin

Typing `log show` fails with "too many arguments" because the zsh builtin `log` is shadowing
`/usr/bin/log`. Always use the full path:
```sh
/usr/bin/log show --debug --info --level debug --predicate 'eventMessage CONTAINS "SMSC95xx"'
```

Alternatively, temporarily disable the builtin:
```sh
enable -n log
log show --debug --info --predicate 'eventMessage CONTAINS "SMSC95xx"'
```

### Dext output is attributed to `kernel`

`os_log` calls from a dext are attributed to the `kernel` process, not the driver. Filtering on
the driver's process name returns nothing. Use the event message or sender image path instead:
```sh
/usr/bin/log show --debug --info --predicate 'eventMessage CONTAINS "SMSC95xx"' --level debug
/usr/bin/log show --debug --info --predicate 'senderImagePath CONTAINS "smsc95xx"' --level debug
```

### Launch failures are below Default level

Activation failures and matching failures come from `kernelmanagerd` and are logged at **Info** or
**Debug** level. Add `--debug --info` to see them:
```sh
/usr/bin/log show --debug --info --predicate 'process == "kernelmanagerd"'
```

### `os_log` redacts plain `%s`

String arguments to `os_log` are redacted as `<private>` unless marked public:
```cpp
os_log(OS_LOG_DEFAULT, "MAC %{public}s", mac_str);  // shows the string
os_log(OS_LOG_DEFAULT, "MAC %s", mac_str);          // shows "MAC <private>"
```

### USB re-enumeration for testing

The `tools/usb-reenumerate` utility forces the USB bus to re-enumerate a device without unplugging it:
```sh
make -C tools/usb-reenumerate
./tools/usb-reenumerate/usb-reenumerate 0424 9905
```

This makes IOKit re-run the matching process. Useful for testing matching rules and `Start()`
without a physical replug. However, once the driver holds the device exclusively, re-enumeration
fails with "Device busy" — this is expected and not an error. To trigger matching again after
driver changes, bump the version in both `Info.plist` files and run `make -C dext install`.

---

## MAC address provenance and correctness

The most important correctness property of this driver: an address is used only when its **provenance**
checks out. `Start()` requires both signals and refuses if either fails —

1. `E2P_CMD` bit 9 (`LOADED`), meaning the chip finished its power-on auto-load; and
2. a `0xA5` signature byte at EEPROM offset 0.

Only then is the address read, and it must additionally pass the plausibility predicate
(`smsc95xx_mac_plausible()` in `common/`, shared with the probe and unit-tested).

On this hardware unit the EEPROM auto-load sometimes fails. *Why* it fails is not established — it has
not been instrumented, and no capture in `reference/` tests any particular cause, so treat any timing
or signal-integrity story as a hypothesis.
When it fails, EEPROM reads return systematically mis-clocked data: each byte appears twice, shifted
left by 1 bit. The mis-read is **stable** — re-reading confirms it, and pattern checks pass it
(valid OUI, not all-ones, not all-zeros). Only the provenance check works: read `LOADED` and the
signature.

**Bad state (auto-load failed):**
- Device reports `0x0424:0x9E00`
- EEPROM offset 0 reads `0x4A` (should be `0xA5`)
- Signature check fails, `start()` returns error
- No address is configured

**Good state (auto-load succeeded):**
- Device reports `0x0424:0x9905` (with strings)
- EEPROM offset 0 reads `0xA5`
- Both provenance checks pass, the address is read, and it is published to the IOKit registry
  (nothing is written to the hardware in M3 — no `ADDRL`/`ADDRH` write, and no network interface exists yet)
- MAC is `fc:61:79:90:04:56` (identical across 10 reads)

The Linux in-tree `smsc95xx` driver checks neither the `LOADED` bit nor the signature, and
`reference/mach-init-sequence.txt` shows it configuring `eth1` with the mis-clocked address
`4a:f8:f8:c2:c2:f2`. This driver refuses the bad state and is correct.

See `reference/m3-dext-match.txt` for the measured evidence and exact byte patterns.

---

## Upgrading the dext

**Detach the device before installing, or the new version will not take effect.**

A dext that holds a device open does not terminate when it is replaced. The upgrade then half-applies:
the new version stages and `systemextensionsctl list` reports it `[activated enabled]`, while the
kernel carries on executing the **old** binary. Observed twice, and the reported state is actively
misleading:

```
$ systemextensionsctl list
*   -  com.github.stewartallen.smsc95xx.driver (1.3/13)  SMSC95xxDriver  [activated enabled]
       com.github.stewartallen.smsc95xx.driver (1.2/12)  SMSC95xxDriver  [terminating for upgrade via delegate]

$ make status
  running: v12 [2D1E556E-...]      <-- the kernel is still running the old one
  staged:  v13 [F58EA073-...]
  declared in tree: v13
```

So the safe sequence is:

1. Unplug the dongle. The dext logs `Stop` then `no services left, exiting` and the stale
   `terminating for upgrade` entry disappears.
2. Bump `CFBundleVersion` in **both** `SMSC95xxDriver/Info.plist` and `SMSC95xxInstaller/Info.plist` —
   without a bump the request is declined as not-newer.
3. `make install`.
4. Plug the dongle back in.

`make status` compares running against staged against declared precisely so this is visible rather
than assumed. `tools/usb-reenumerate` cannot substitute for the unplug: once our driver owns the
device it fails with `kIOReturnExclusiveAccess`.

Whether holding a device open is strictly the *cause* of the stalled termination is not proven — one
upgrade did succeed while the driver was present but failing `Start()`, and so holding nothing. The
workaround above is what is established.

Version bumps are required for re-installation. After editing the driver, bump `CFBundleVersion`
in both `Info.plist` files:
- `SMSC95xxDriver/Info.plist`
- `SMSC95xxInstaller/Info.plist`

Then reinstall:
```sh
make install
```

(No `sudo`: it is not needed, and building as root leaves root-owned files in `build/` that break the
next unprivileged `make`.)

A version bump is necessary but **not sufficient**. If the running dext holds a device open it does not
terminate, so the new version stages without taking effect — see the detach step above. `make status`
compares running against staged so you can tell.

Do **not** replace an installed dext (with `cp` or by deleting and installing) without bumping
the version. Without a version increase, the system silently declines the replacement, leaving
the old dext in place. You will see "activation succeeded" in the installer output while the old
driver remains active — a subtle and misleading failure mode.

---

## Not distributable

This driver is built for **local development only**. It requires:
- SIP disabled (security regression)
- AMFI validation disabled (security regression)
- Unsigned ad-hoc signatures (impossible for distribution)
- Restricted entitlements that require Apple's approval, which this maintainer has not yet obtained

Making this driver ready for distribution is out of scope for this project.
