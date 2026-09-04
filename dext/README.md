# Building and loading SMSC95xxDriver.dext

> **Scope as of M5.** This driver moves frames. `ping` works in both directions between the Mac and
> a peer on the T1S segment — see `reference/m5-datapath.txt` for the measured evidence. One USB bulk
> transfer is outstanding in each direction at a time and every frame is copied once, so M5 is
> correctness rather than throughput; a ring and `AsyncIOBundled` are M6.
>
> Still do not read `status: active` as reachability. It is reported for as long as the dongle is
> attached rather than polled, because a T1S segment without PLCA carries no continuous
> link-integrity signal — see the link-state section for why.


The DriverKit extension (`dext`) for the LAN9500A is built entirely with the `Makefile` — no Xcode
project. Every non-obvious flag has been validated on hardware and is documented here.

---

## Prerequisites

With a **paid Apple Developer account** (development signing identity, two provisioning profiles)
the dext loads on a stock Mac: SIP enabled, Full Security, developer mode off. Verified on macOS
26.6 (Darwin 25.6), M4 Pro. Without an account, see
[Without a developer account](#without-a-developer-account).

1. **Create the App IDs and profiles** — see [Signing with a real identity](#signing-with-a-real-identity).

2. **Configure signing:** copy `.env.example` to `.env` and fill in the identity and profile paths.

3. **Install:**
   ```sh
   make install
   ```
   No `sudo`. Approve the extension when System Settings prompts (General > Login Items &
   Extensions > Driver Extensions) and enter your password — that approval completes the
   activation.

4. **Verify the security posture** — a stale value in any of these invalidates a load result:
   ```sh
   csrutil status                                                      # "enabled"
   nvram boot-args                                                     # "data was not found"
   plutil -p /Library/SystemExtensions/db.plist | grep developerMode   # false
   ```

### No AMFI boot-arg, no SIP change

Earlier revisions of this driver required `sudo nvram boot-args="amfi_get_out_of_my_way=0x1"`,
because an ad-hoc signature cannot authorise the restricted entitlements the driver and its
installer claim. Signing with a real Apple Development identity plus embedded provisioning profiles
authorises them properly, and the driver loads with AMFI enforcing. If you still have the boot-arg
set from an earlier revision, remove it:

```sh
sudo nvram -d boot-args   # then reboot
```

SIP does **not** need to be disabled either: a development-signed, profile-backed dext in
`/Applications` loads with SIP enabled and Full Security. `systemextensionsctl developer` is not
part of this flow — it refuses to run while SIP is on. What limits where this build loads is the
provisioning profile, not SIP or AMFI; see [Not distributable](#not-distributable).

### Without a developer account

Without a paid account an ad-hoc signature cannot authorise the restricted entitlements, so you
disable the entitlement check instead: **reduced security plus the AMFI boot-arg**.

```sh
# 1. Recovery (hold the power button, Options, Utilities, Terminal):
csrutil disable            # also drops the Mac to Reduced Security, which is what
                           # permits setting boot-args at all on Apple Silicon
reboot

# 2. Back in macOS, set the boot-arg, then reboot again so it takes effect:
sudo nvram boot-args="amfi_get_out_of_my_way=0x1"
reboot

# 3. Build and install with no .env at all — both bundles come out ad-hoc signed:
make install
```

Do not run `systemextensionsctl developer on`: it only relaxes the `/Applications` location check,
which `make install` already satisfies, and it does not survive a reboot.

**Warning:** `amfi_get_out_of_my_way=0x1` disables code-signing entitlement validation
**machine-wide**, not just for this driver, and it breaks unrelated software that depends on it —
JDKs and .NET runtimes were both affected on this machine.

This is the path the project itself used before 2026-08-30; the steps have not been re-verified
since the switch to signing. If they fail, check `nvram boot-args` and `csrutil status` first —
both silently revert with an OS update or a security-policy change — then see Debugging, below.

---

## Building and installing

All commands run from this directory (`dext/`). Every target re-builds prerequisites automatically.

### `make` — build the dext and installer app

```sh
cd dext
make
```

Builds `build/com.github.stewartallen.smsc95xx.driver.dext` and
`build/SMSC95xxInstaller.app`. The build system:
- Runs `iig` on the C++ interface definition to generate the vtable glue
- Compiles all code with `arm64e` (pointer authentication, required on Apple Silicon)
- Links against `DriverKit` and `USBDriverKit` frameworks
- Signs both bundles with full entitlements

With no `.env` and no arguments both bundles are **ad-hoc** signed, which does not authorise their
restricted entitlements — such a build only loads with the AMFI boot-arg, which cannot be set on a
Mac in Full Security. Create `dext/.env` from `.env.example` to sign properly; see
[Signing with a real identity](#signing-with-a-real-identity).

**Build artifacts are placed in `build/`, not in the source tree or Xcode default locations.**

### `make install` — stage and activate the dext

```sh
make install
```

Copies the installer app to `/Applications` and runs it to activate the dext. On completion:
- `systemextensionsctl list` will show the dext as enabled and active
- `ioreg` will show a matching `IOUserService` under the device node
- Kernel logs will show matching and `Start()` messages

No `sudo` — an admin user can write `/Applications` directly. Activation is authorised
interactively: approve the extension when System Settings prompts (General > Login Items &
Extensions > Driver Extensions) and enter your password. The activation is not complete until you
do; the `make` target returning is not the finish line.

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
make uninstall
```

Deactivates the dext and removes the installer app from `/Applications`. No `sudo`, but expect a
password prompt. Kernel may take a few seconds to fully tear down the driver after deactivation;
the installer reports `OK: completes after reboot` when teardown is deferred.

### `make clean` — remove all build artifacts

```sh
make clean
```

Removes the entire `build/` directory.

### `make TRACE=1` — compile in the per-frame datapath tracing

```sh
make app TRACE=1
```

Off by default. A normal build logs errors, refusals, the guards, and two lines of TX/RX
totals whenever a run of traffic ends — enough to answer *did it work and did it lose
anything*. `TRACE=1` adds three per-call traces that answer *what happened to this
particular frame*:

| Prefix | Covers |
|---|---|
| `DIAG5:` | every NetworkingDriverKit callback into the driver, with arguments and the value returned |
| `DIAG6:` | transmit: every doorbell, dequeue, framed transfer, completion, and packet handed back |
| `DIAG7:` | receive: every completion, record, drop with its reason, enqueue, and re-arm |

```sh
/usr/bin/log stream --predicate 'eventMessage CONTAINS "DIAG6:"' --style compact
```

**Why it is not on by default.** `os_log` throttles hard. During M5 a burst of ~364,000
messages caused *later* attach-time logs to vanish from the store entirely — worse than
having no tracing at all, because the absence looks like the driver never ran. The trace is
also 34 KB of format strings in the binary.

Disabled, the macros compile to `if (0) os_log(...)` rather than to nothing, so every format
string and argument is still type-checked in a default build and a trace line cannot rot
unnoticed. That relies on no trace call having a side effect in its arguments — currently
true of all 68 of them, and worth keeping true.

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

The dext (`SMSC95xxDriver/SMSC95xxDriver.entitlements.in`) carries three:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.transport.usb`
- `com.apple.developer.driverkit.family.networking` — claimed ahead of need, so an entitlement
  rejection surfaces now rather than at M4 alongside a new queue model

The installer app (`SMSC95xxInstaller/Installer.entitlements.in`) carries one:

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
directory exits 2 and prints usage once re-signed *without* the entitlement.

---

## Signing with a real identity

Restricted entitlements are authorised by a **provisioning profile**, not by the certificate. A real
Apple Development certificate alone changes nothing; the profile is what grants the entitlements, and
it has to be embedded in the bundle.

### What to create in the developer portal

Two App IDs, two profiles:

| | App ID | Profile type | Capabilities to tick |
|---|---|---|---|
| dext | `com.github.stewartallen.smsc95xx.driver` | DriverKit App Development | DriverKit, DriverKit Family Networking, DriverKit USB Transport |
| installer | `com.github.stewartallen.smsc95xx` | macOS App Development | System Extension |

Both profiles must list this Mac's **Provisioning UDID** — not its hardware UUID or serial:

```sh
system_profiler SPHardwareDataType | grep "Provisioning UDID"
```

Check what a downloaded profile actually grants with `tools/inspect-profile/inspect-profile.sh`.
Note that a DriverKit profile's `Platform` field reads `OSX`, not `DriverKit`; the App ID and the
granted entitlements are what identify it.

### Building signed

Signing configuration lives in `dext/.env`, which is **gitignored** — it names a person and points
at files that must never be committed. Copy the template and fill it in:

```sh
cp .env.example .env
```

```sh
SIGN_ID=Apple Development: you@example.com (XXXXXXXXXX)
DEXT_PROFILE=~/Downloads/YourDriver_DriverKit_Development.provisionprofile
APP_PROFILE=~/Downloads/YourInstaller_macOS_Development.provisionprofile
```

`.env` is read by `make`, not by a shell, so values are taken literally — **do not quote them**, and
`$VAR` is not expanded. The Makefile strips stray quotes and expands a leading `~/` anyway, because
both are easy to write by habit and both fail confusingly. Then just:

```sh
make install
```

The same variables can still be passed per invocation, which overrides `.env`:

```sh
make install SIGN_ID="Apple Development: …" DEXT_PROFILE=… APP_PROFILE=…
```

**No Apple Team ID is checked in.** The two entitlements files are templates
(`*.entitlements.in`) carrying a `__TEAM_ID__` placeholder; the Makefile reads the real value out of
the provisioning profile and writes the substituted files into `build/gen/`. Reading it from the
profile rather than hardcoding it means it cannot drift from the profile it has to agree with, and
it lets anyone build this with their own account. `TEAM_ID` can be set explicitly in `.env` if you
are signing without a profile.

Three ways to get this wrong are caught at build time rather than at launch, where a signing fault
is far more expensive to diagnose: a profile path that does not exist, `SIGN_ID` set with no Team ID
available, and a placeholder that survived substitution. A bare `make` with no `.env` still produces
an ad-hoc build.

`make signinfo` reports the resulting Authority chain and whether a profile is embedded in each
bundle. A correct dext reads:

```
Authority=Apple Development: you@example.com (XXXXXXXXXX)
Authority=Apple Worldwide Developer Relations Certification Authority
Authority=Apple Root CA
TeamIdentifier=<your team>
  profile: embedded
```

Two entitlements exist purely to tie the binary to its profile and must match it exactly:
`com.apple.application-identifier` (team ID + bundle ID) and `com.apple.developer.team-identifier`.

### The entitlement value must match the profile exactly

This is the part that is not documented and cost the most time. The DriverKit App Development profile
grants:

```
com.apple.developer.driverkit.transport.usb = ( { idVendor = "*" } )
```

That `"*"` is **not** a wildcard the checker expands and matches against concrete values. It is a
literal that must be reproduced verbatim. Measured against this profile:

| `transport.usb` claimed by the binary | Verdict |
|---|---|
| *(key absent — control)* | accepted |
| `[{idVendor: "*"}]` | **accepted** |
| `[{idVendor: 1060}, {idVendor: 6223}]` | rejected |
| `[{idVendor: "*", idProduct: "*"}]` | rejected |
| `[{idVendor: "1060"}]` | rejected |
| `true` | rejected |

So the entitlement cannot be narrowed to our actual vid/pid pairs under a development profile.
This is safe: the entitlement is only an upper bound on what the dext is *allowed* to attach to,
while the `Info.plist` personalities decide what it *does* attach to, and IOKit will not hand it
anything else. Narrowing the entitlement itself needs a Developer ID profile with values approved by
Apple — the known route, untested here.

`tools/probe-entitlements/probe-entitlements.sh` tests a candidate entitlements file against a
profile without installing the extension or touching the hardware, which turns a
replug-and-pray loop into a few seconds. Always include a control case with the restricted
entitlement absent; it must come back accepted, or the harness is not measuring anything.

### Diagnosing a rejection

The error surfaced at the top is misleading in two ways. `kernelmanagerd` reports:

```
Error Domain=NSPOSIXErrorDomain Code=8 "Exec format error"
```

which is the *same* message a wrong-architecture dext produces (see
[Why `arm64e` and not `arm64`](#why-arm64e-and-not-arm64)). And AMFI reports:

```
Error Domain=AppleMobileFileIntegrityError Code=-413 "No matching profile found"
```

which reads as "the profile is missing" even when the profile was found, verified and matched to
this Mac. The line that actually says what is wrong comes from `taskgated-helper`:

```sh
/usr/bin/log show --last 5m \
    --predicate 'process == "amfid" OR process == "taskgated-helper"' --style compact
```

```
Checking profile: <your profile name>
ProfileGrantsEntitlement matchEntitlement failed value match for entitlement:
    com.apple.developer.driverkit.transport.usb
Unsatisfied entitlements: com.apple.developer.driverkit.transport.usb
Disallowing: ...
```

If the log contains `Unsatisfied Entitlements`, it is a profile mismatch and not an architecture
problem. Beware that **`codesign -v` reports "valid on disk" either way** — the signature genuinely
is valid, and profile authorisation is a separate check that amfid performs at exec time. Verifying
the Authority chain is necessary but not sufficient.

One build-time trap: `codesign` parses the entitlements plist with `AMFIUnserializeXML`, which is
stricter than `plutil`. A double hyphen inside an XML comment passes `plutil -lint` and then fails
signing with `AMFIUnserializeXML: syntax error near line N`.

Full measured evidence: `reference/signing-entitlement-match.txt`.

---

## Device matching and the provider

### Two drivers: why both provider classes are needed

Since M5 there are two classes, and five personalities across them:

| Class | Provider | `IOClass` | `CFBundleIdentifierKernel` | Job |
|---|---|---|---|---|
| `SMSC95xxUSBDevice` | `IOUSBHostDevice` | `IOUserService` | `com.apple.kpi.iokit` | select configuration 1, nothing else |
| `SMSC95xxDriver` | `IOUSBHostInterface` | `IOUserNetworkEthernet` | `com.apple.iokit.IOSkywalkFamily` | pipes, registers, network interface |

The device arrives at the macOS USB stack **unconfigured** — with configuration 0 selected. See
`reference/m3-attach-state.txt` for the measured state at clean attach. Unconfigured devices have no
interface nodes in the IOKit tree, only a device node, so an interface personality on its own can
never match: the dext builds, signs, installs and activates cleanly, then silently never matches with
nothing logged to say why. That is what the device-level class exists to prevent — it calls
`SetConfiguration(1, true)`, and `matchInterfaces` must be `true` there, because the interface nodes
that call creates are exactly what `SMSC95xxDriver` then matches against.

The bulk endpoints live on the interface, not the device, which is why the datapath belongs on that
half. The interface in the provider chain is also what lets SystemConfiguration name the port from
`kUSBProductString` instead of calling it `Ethernet Adapter (enN)`.

The interface-level driver needs no device handle of its own: control transfers for registers, MII and
EEPROM go through `interface->DeviceRequest(..., kIOUSBDeviceRequestRecipientDevice, ...)`.

**Interface match keys fail silently if wrong.** A personality specifying `idVendor` MUST also specify
`idProduct` or `idProductArray`, plus `bConfigurationValue` and `bInterfaceNumber`. `idVendor` alone is
rejected before matching and logs nothing at any level, including `--debug --info`. All 121 of Apple's
own `IOUSBHostInterface` personalities were surveyed and none uses `idVendor` without a product key.
Ours uses `idProductArray` for the MACH, whose USB product ID depends on whether its EEPROM auto-load
succeeded. Full evidence: `reference/m5-interface-matching.txt`.

Note also that **`ioreg -p IOUSB` does not show `IOUSBHostInterface` nodes** as children of a device.
Use the IOService plane (plain `ioreg`) and parse `ioreg -a` as a plist; scraping the indented text
form misattributes properties to the wrong node.

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

## Advertised offloads — closed, and they are cosmetic

The interface advertises `TSO4,TSO6,PARTIAL_CSUM` — TCP segmentation and partial checksum offload —
that the driver does not implement. `Start()` never touches `COE_CR`, and there is no segmentation
logic anywhere in the driver. At M4 this was an open item and a real risk to the datapath: if the
stack were handing down TSO segments, transmission would have failed in a way that looked exactly
like a framing bug.

**Measured at M5, before any framing code existed** — which was the point. The TX dequeue handler
logged the length of every packet the stack handed us and completed it without transmitting:

```
lengths observed:  42 .. 1372
maximum:           1372
over 1514:         0
```

So Skywalk performs those offloads in software above the driver. The driver is never handed a
segmented or partially-checksummed frame, and the advertisement is cosmetic. **This item is closed.**
That confirms the hypothesis recorded at M4 — that `options=` reflects what the Skywalk layer offers
the BSD stack rather than a claim about our hardware, which `CHANNEL_IO` and `ZEROINVERT_CSUM`
appearing in the same list already suggested, both being Skywalk-native flags.

Three driver-side APIs were tried first, each returning nothing:
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

The 1518-byte length guard in the transmit path stays regardless. That is a memory-safety bound on a
`memcpy`, not an offload question.

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

This driver loads on a stock, fully secured Mac — but only on **yours**. It requires:
- A development provisioning profile naming a specific Mac's Provisioning UDID, so a copy of
  someone else's build will not load on your machine; you build it yourself, with your own account
- A `transport.usb` entitlement that a development profile forces to `idVendor: "*"`, which is far
  broader than the three devices this driver actually matches

Distribution would need a Developer ID identity, notarisation, and Apple's approval of concrete
vid/pid values for the USB transport entitlement. Apple grants those by request
(<https://developer.apple.com/system-extensions/>) but scopes the grant to the company that owns
the hardware vendor ID — `0424` belongs to Microchip and `184f` to its subsidiary K2L, not to this
project. Building it yourself is the supported path.
