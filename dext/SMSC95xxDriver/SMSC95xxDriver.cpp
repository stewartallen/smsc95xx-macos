/* SPDX-License-Identifier: GPL-2.0 */

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/AppleUSBDefinitions.h>
#include <NetworkingDriverKit/IOUserNetworkEthernet.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>
#include <DriverKit/IODispatchQueue.h>

#include "SMSC95xxDriver.h"
#include "SMSC95xxDriver_ivars.h"
#include "smsc95xx_regs.h"
#include "smsc95xx_proto.h"
/* The chip initialisation sequence, shared verbatim with the userspace probe so the two
 * cannot drift. It performs no I/O of its own: this file supplies register access through
 * the callbacks below. */
#include "smsc95xx_init.h"

/* One definition, used by both reportLinkStatus call sites and getSupportedMediaArray,
 * so the advertised media and the reported active media cannot drift apart.
 * 10BASE-T is the closest available word -- there is no 10BASE-T1S constant -- and the
 * segment is genuinely half duplex. */
#define SMSC95XX_MEDIA_WORD \
    (kIOUserNetworkMediaEthernet10BaseT | kIOUserNetworkMediaOptionHalfDuplex)

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: " fmt, ##__VA_ARGS__)

/* The DIAG5 trace: what NetworkingDriverKit asks of this driver, and what we answer.
 * Compiled in only with `make TRACE=1`; see the TRACE block in the Makefile for why it is
 * off by default. One prefix, so a single predicate selects the whole trace:
 *   log stream --predicate 'eventMessage CONTAINS "DIAG5:"'
 *
 * The disabled form is `if (0) os_log(...)`, not an empty macro, so that format strings and
 * arguments stay type-checked in every build -- a trace line that no longer compiles is
 * exactly the kind of rot that makes people delete instrumentation instead of using it. None
 * of the 68 call sites has a side effect in its arguments, which is what makes discarding
 * the evaluation safe; keep it that way.
 *
 * Pointers are logged as 0x%llx through (uint64_t)(uintptr_t) rather than %p: %p wants a
 * void * and -Wformat objects to anything else, and the casts would then have to strip
 * const on some of these arguments. */
#if SMSC95XX_TRACE
#define Diag(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG5: " fmt, ##__VA_ARGS__)
#else
#define Diag(fmt, ...) \
    do { if (0) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG5: " fmt, ##__VA_ARGS__); } while (0)
#endif
#define DiagPtr(p)     ((uint64_t)(uintptr_t)(p))

/* Poll attempts for MII and EEPROM operations */
#define POLL_ATTEMPTS 100

/* Depth of each of the four packet queues. M4 passed the same 64 to all of them, so name it
 * once here rather than repeat the literal. 64 is generous for a 10 Mb/s half-duplex segment
 * with one transfer in flight -- depth buys nothing here -- and matches the pool's packet
 * count, which is the real ceiling on packets in flight. Lives in this file because this is
 * the only file that builds queues; the datapath's drain batch is sized independently. */
#define SMSC95XX_TX_QUEUE_CAPACITY 64

/* Per-role queue ids, taken from the IOReport legend a working driver publishes
 * (AppleBCMWLAN): submission queues share id 0 across the two directions, while the
 * completion queues take 4 and 5. All four were 0 here until now. */
#define SMSC95XX_QID_TX_SUBMIT     0
#define SMSC95XX_QID_RX_SUBMIT     0
#define SMSC95XX_QID_TX_COMPLETE   4
#define SMSC95XX_QID_RX_COMPLETE   5

/* SMSC95xxDriver_IVars is defined in SMSC95xxDriver_ivars.h, shared with
 * SMSC95xxDriver_datapath.cpp -- the class spans two translation units and both need the
 * layout. That header also carries the note on why there is no device handle. */

/* Private helper for tearing down resources on Start() failure or Stop(). */
static kern_return_t
TearDown(SMSC95xxDriver *driver, IOService *provider);

bool
SMSC95xxDriver::init(void)
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(SMSC95xxDriver_IVars, 1);
    return ivars != nullptr;
}

void
SMSC95xxDriver::free(void)
{
    IOSafeDeleteNULL(ivars, SMSC95xxDriver_IVars, 1);
    super::free();
}

/* Vendor control transfers, identical in shape to the probe's usb_control_in/out:
 * 0xA1 reads a register, 0xA0 writes one, the offset rides in wIndex, and the
 * payload is a little-endian u32. */
kern_return_t
SMSC95xxDriver::readRegister(uint32_t offset, uint32_t *value)
{
    uint16_t transferred = 0;
    kern_return_t ret;

    if (value == nullptr || ivars->interface == nullptr || ivars->ctrlBuffer == nullptr) {
        return kIOReturnBadArgument;
    }

    ret = ivars->interface->DeviceRequest(
        (uint8_t)(kIOUSBDeviceRequestDirectionIn | kIOUSBDeviceRequestTypeVendor | kIOUSBDeviceRequestRecipientDevice),
        SMSC95XX_REQ_READ_REGISTER,
        0,
        (uint16_t)offset,
        sizeof(uint32_t),
        ivars->ctrlBuffer,
        &transferred,
        1000);
    if (ret != kIOReturnSuccess) {
        Log("read 0x%03x failed: 0x%x", offset, ret);
        return ret;
    }
    if (transferred != sizeof(uint32_t)) {
        Log("read 0x%03x short: %u of 4 bytes", offset, transferred);
        return kIOReturnUnderrun;
    }

    *value = ((uint32_t)ivars->ctrlBytes[0])
           | ((uint32_t)ivars->ctrlBytes[1] << 8)
           | ((uint32_t)ivars->ctrlBytes[2] << 16)
           | ((uint32_t)ivars->ctrlBytes[3] << 24);
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::writeRegister(uint32_t offset, uint32_t value)
{
    uint16_t transferred = 0;
    kern_return_t ret;

    if (ivars->interface == nullptr || ivars->ctrlBuffer == nullptr) {
        return kIOReturnBadArgument;
    }

    ivars->ctrlBytes[0] = (uint8_t)(value & 0xFF);
    ivars->ctrlBytes[1] = (uint8_t)((value >> 8) & 0xFF);
    ivars->ctrlBytes[2] = (uint8_t)((value >> 16) & 0xFF);
    ivars->ctrlBytes[3] = (uint8_t)((value >> 24) & 0xFF);

    ret = ivars->interface->DeviceRequest(
        (uint8_t)(kIOUSBDeviceRequestDirectionOut | kIOUSBDeviceRequestTypeVendor | kIOUSBDeviceRequestRecipientDevice),
        SMSC95XX_REQ_WRITE_REGISTER,
        0,
        (uint16_t)offset,
        sizeof(uint32_t),
        ivars->ctrlBuffer,
        &transferred,
        1000);
    if (ret != kIOReturnSuccess) {
        Log("write 0x%03x failed: 0x%x", offset, ret);
        return ret;
    }
    if (transferred != sizeof(uint32_t)) {
        Log("write 0x%03x short: %u of 4 bytes", offset, transferred);
        return kIOReturnUnderrun;
    }
    return kIOReturnSuccess;
}

/* Poll a register until the given mask clears. Helper for miiRead/eepromRead. */
static kern_return_t
wait_clear(SMSC95xxDriver *driver, uint16_t offset, uint32_t mask)
{
    for (int i = 0; i < POLL_ATTEMPTS; i++) {
        uint32_t v = 0;
        kern_return_t kr = driver->readRegister(offset, &v);
        if (kr != kIOReturnSuccess)
            return kr;
        if (!(v & mask))
            return kIOReturnSuccess;
    }
    return kIOReturnTimeout;
}

kern_return_t
SMSC95xxDriver::miiRead(uint8_t phy, uint8_t reg, uint16_t *value)
{
    kern_return_t kr = wait_clear(this, SMSC95XX_REG_MII_ADDR, SMSC95XX_MII_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    kr = writeRegister(SMSC95XX_REG_MII_ADDR,
                       smsc95xx_mii_addr_word(phy, reg, false));
    if (kr != kIOReturnSuccess)
        return kr;

    kr = wait_clear(this, SMSC95XX_REG_MII_ADDR, SMSC95XX_MII_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    uint32_t data = 0;
    kr = readRegister(SMSC95XX_REG_MII_DATA, &data);
    if (kr == kIOReturnSuccess)
        *value = (uint16_t)(data & 0xFFFFu);
    return kr;
}

kern_return_t
SMSC95xxDriver::eepromRead(uint16_t offset, uint8_t *buf, size_t len)
{
    /* EEPROM addresses are 9 bits; reject out-of-range requests. */
    if (offset + len > 0x200)
        return kIOReturnBadArgument;

    kern_return_t kr = wait_clear(this, SMSC95XX_REG_E2P_CMD, SMSC95XX_E2P_BUSY);
    if (kr != kIOReturnSuccess)
        return kr;

    for (size_t i = 0; i < len; i++) {
        kr = writeRegister(SMSC95XX_REG_E2P_CMD,
                           smsc95xx_e2p_read_cmd((uint16_t)(offset + i)));
        if (kr != kIOReturnSuccess)
            return kr;

        /* Read back E2P_CMD to confirm the operation retired and did not time
         * out. The captured trace does exactly this between each byte. */
        uint32_t cmd = 0;
        bool retired = false;
        for (int attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
            kr = readRegister(SMSC95XX_REG_E2P_CMD, &cmd);
            if (kr != kIOReturnSuccess)
                return kr;
            if (!(cmd & SMSC95XX_E2P_BUSY)) {
                retired = true;
                break;
            }
        }
        if (!retired)
            return kIOReturnTimeout;
        if (cmd & SMSC95XX_E2P_TIMEOUT)
            return kIOReturnNotFound;   /* no EEPROM responding */

        uint32_t data = 0;
        kr = readRegister(SMSC95XX_REG_E2P_DATA, &data);
        if (kr != kIOReturnSuccess)
            return kr;
        buf[i] = (uint8_t)(data & 0xFFu);
    }
    return kIOReturnSuccess;
}

/* ---------------------------------------------------------------------------------------
 * Chip initialisation.
 *
 * The sequence itself lives in common/smsc95xx_init.c, shared verbatim with the probe --
 * it is the one thing that has to be identical in both, and it is what actually switches
 * receive and transmit on in hardware by writing MAC_CR. What lives here is the adapter:
 * two thunks that turn the shared layer's (ctx, offset, value) callbacks into this class's
 * readRegister/writeRegister, plus per-access logging so that a failed init is
 * diagnosable from a single attach rather than needing a USB capture.
 * ------------------------------------------------------------------------------------- */

/* Carried through the shared layer as its opaque `ctx`. The step counter is only for the
 * log: it turns "a write failed" into "the eleventh write, AFC_CFG, failed". */
struct SMSC95xxInitIO {
    SMSC95xxDriver *driver;
    uint32_t        reads;
    uint32_t        writes;
};

static int
initRegRead(void *ctx, uint16_t offset, uint32_t *value)
{
    SMSC95xxInitIO *io = static_cast<SMSC95xxInitIO *>(ctx);
    io->reads++;

    kern_return_t ret = io->driver->readRegister(offset, value);
    if (ret != kIOReturnSuccess) {
        /* readRegister already logs the transport failure; this line adds where in the
         * sequence it happened, which is the part that identifies the register. */
        Log("init: read #%u %{public}s (0x%03x) FAILED: 0x%x", io->reads,
            smsc95xx_reg_name(offset), offset, ret);
    } else {
        Log("init: read #%u %{public}s (0x%03x) -> 0x%08x", io->reads,
            smsc95xx_reg_name(offset), offset, *value);
    }
    /* Returned unchanged: an IOReturn is 0 on success and non-zero otherwise, which is
     * exactly the smsc95xx_io contract, so the shared layer propagates a real IOReturn
     * back out to Start() without knowing what one is. */
    return (int)ret;
}

static int
initRegWrite(void *ctx, uint16_t offset, uint32_t value)
{
    SMSC95xxInitIO *io = static_cast<SMSC95xxInitIO *>(ctx);
    io->writes++;

    kern_return_t ret = io->driver->writeRegister(offset, value);
    /* EVERY write with its value, unconditionally. This is the register trace the probe
     * had and the driver did not, and it is what makes an init that fails on real
     * hardware readable straight out of `log stream`. */
    Log("init: write %u/%u %{public}s (0x%03x) = 0x%08x -> 0x%x", io->writes,
        (unsigned int)SMSC95XX_INIT_WRITE_COUNT, smsc95xx_reg_name(offset), offset,
        value, ret);
    return (int)ret;
}

/* Map the shared layer's own two failure codes onto IOReturn. Anything else already IS an
 * IOReturn, because it came back out of one of the thunks above. */
static kern_return_t
initSeqResultToIOReturn(int rc)
{
    switch (rc) {
    case 0:                               return kIOReturnSuccess;
    case SMSC95XX_INIT_ERR_BAD_ARG:       return kIOReturnBadArgument;
    case SMSC95XX_INIT_ERR_RESET_TIMEOUT: return kIOReturnTimeout;
    default:                              return (kern_return_t)rc;
    }
}

/* Teardown path for partial Start() failures. Must be safe to call after any
 * subset of ivars has been populated. Called from both the fail: label in Start()
 * and from Stop(). The order is fixed: the datapath first, because an in-flight bulk
 * transfer references the RX buffer; then the four queues before the pool, because they
 * hold references to it, and tearing the pool down first is the kind of ordering bug that
 * only shows up on an error path; then the interface; then the dispatch queue last. */
static kern_return_t
TearDown(SMSC95xxDriver *driver, IOService *provider __unused)
{
    /* The datapath goes FIRST, before the queues and the pool: an in-flight bulk transfer
     * references the RX buffer, and teardownDatapath aborts both pipes synchronously
     * before releasing anything. Nothing below may run while a transfer is still live. */
    driver->teardownDatapath();

    /* Release queues before the pool (they hold references to it). */
    OSSafeReleaseNULL(driver->ivars->txSubmit);
    OSSafeReleaseNULL(driver->ivars->rxSubmit);
    OSSafeReleaseNULL(driver->ivars->txComplete);
    OSSafeReleaseNULL(driver->ivars->rxComplete);

    /* Release the pool after the queues. */
    OSSafeReleaseNULL(driver->ivars->pool);

    if (driver->ivars->interface != nullptr) {
        if (driver->ivars->interfaceOpened) {
            driver->ivars->interface->Close(driver, 0);
            driver->ivars->interfaceOpened = false;
        }
        /* The interface is the matched provider, retained by IOKit for as long as we
         * are attached: close it (above, only if we opened it), never release it, and
         * null it so a second TearDown cannot issue a second Close. */
        driver->ivars->interface = nullptr;
    }
    driver->ivars->ctrlBytes = nullptr;
    OSSafeReleaseNULL(driver->ivars->ctrlBuffer);

    /* Release the dispatch queue last. This drops the +1 CopyDispatchQueue handed us, not
     * the service's own reference: the Default queue was created by DriverKit in
     * IOService::init() and outlives this, which is why callbacks can still be delivered
     * on it while Stop unwinds. */
    OSSafeReleaseNULL(driver->ivars->dispatchQueue);

    return kIOReturnSuccess;
}

kern_return_t
IMPL(SMSC95xxDriver, Start)
{
    /* DIAG5 (temporary): anchors for the call sequence. Everything the family asks of us
     * during registration happens between these two lines, so having them lets the trace
     * be read in order rather than inferred. */
    Diag("Start(provider=0x%llx) entered -- before Start(SUPERDISPATCH)", DiagPtr(provider));
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    Diag("Start: Start(SUPERDISPATCH) returned 0x%x", ret);
    if (ret != kIOReturnSuccess) {
        Log("Start(super) failed: 0x%x", ret);
        return ret;
    }

    Log("Start: matched provider");

    /* Both are function-scoped because the provenance checks below jump out of their block:
     * macRejectReason names which gate failed, for the one refusal line at mac_rejected, and
     * eeprom_loaded is read at mac_publish so ioreg reports real hardware state. */
    const char *macRejectReason = nullptr;
    bool eeprom_loaded = false;

    ivars->interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (ivars->interface == nullptr) {
        Log("provider is not an IOUSBHostInterface");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    /* The interface is the only USB object this class needs. Register, MII and EEPROM
     * access is addressed to the device, but IOUSBHostInterface::DeviceRequest with
     * kIOUSBDeviceRequestRecipientDevice is what carries it there, so no device handle is
     * required -- and configuration selection, the one thing that would need one, belongs
     * to SMSC95xxUSBDevice now. We therefore do not CopyDevice and do not call
     * SetConfiguration here.
     *
     * IOUSBHostInterface::Open takes uint8_t * for arg, not uintptr_t. */
    ret = ivars->interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        Log("interface Open failed: 0x%x", ret);
        goto fail;
    }
    ivars->interfaceOpened = true;

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, sizeof(uint32_t), 0,
                                           &ivars->ctrlBuffer);
    if (ret != kIOReturnSuccess) {
        Log("control buffer allocation failed: 0x%x", ret);
        goto fail;
    }
    {
        /* GetAddressRange, not Map: Map creates an IOMemoryMap and returns only the
         * address, leaving no handle to release at teardown. GetAddressRange yields
         * the buffer address with no mapping object to own. */
        IOAddressSegment range = {};
        ret = ivars->ctrlBuffer->GetAddressRange(&range);
        if (ret != kIOReturnSuccess) {
            Log("control buffer address lookup failed: 0x%x", ret);
            goto fail;
        }
        if (range.length < sizeof(uint32_t)) {
            Log("control buffer too small: %llu bytes", range.length);
            ret = kIOReturnNoMemory;
            goto fail;
        }
        ivars->ctrlBytes = reinterpret_cast<uint8_t *>(range.address);
    }

    {
        uint32_t idRev = 0;
        uint16_t chip = 0, rev = 0;

        ret = readRegister(SMSC95XX_REG_ID_REV, &idRev);
        if (ret != kIOReturnSuccess) {
            goto fail;
        }
        smsc95xx_id_rev_split(idRev, &chip, &rev);
        Log("ID_REV 0x%08x chip 0x%04x rev 0x%04x", idRev, chip, rev);

        /* Publish ID_REV to ioreg via registry properties */
        OSDictionary *props = OSDictionary::withCapacity(1);
        if (props != nullptr) {
            OSNumber *number = OSNumber::withNumber(idRev, 32);
            if (number != nullptr) {
                props->setObject("SMSC95xxIDRev", number);
                if (SetProperties(props) != kIOReturnSuccess) {
                    Log("SetProperties failed; registry properties will be absent");
                }
                OSSafeReleaseNULL(number);
            }
            OSSafeReleaseNULL(props);
        }
    }

    /* Read PHY ID (PHYID1 and PHYID2) */
    {
        uint16_t phyid1 = 0, phyid2 = 0;

        ret = miiRead(SMSC95XX_PHY_ADDR, SMSC95XX_MII_PHYID1, &phyid1);
        if (ret != kIOReturnSuccess) {
            Log("PHYID1 read failed: 0x%x", ret);
            goto fail;
        }

        ret = miiRead(SMSC95XX_PHY_ADDR, SMSC95XX_MII_PHYID2, &phyid2);
        if (ret != kIOReturnSuccess) {
            Log("PHYID2 read failed: 0x%x", ret);
            goto fail;
        }

        uint32_t phyid = ((uint32_t)phyid1 << 16) | phyid2;
        Log("PHY ID 0x%04x:0x%04x", phyid1, phyid2);

        /* Publish PHY ID to ioreg */
        OSDictionary *props = OSDictionary::withCapacity(1);
        if (props != nullptr) {
            OSNumber *number = OSNumber::withNumber(phyid, 32);
            if (number != nullptr) {
                props->setObject("SMSC95xxPHYID", number);
                if (SetProperties(props) != kIOReturnSuccess) {
                    Log("SetProperties failed; registry properties will be absent");
                }
                OSSafeReleaseNULL(number);
            }
            OSSafeReleaseNULL(props);
        }
    }

    /* Check BMSR for autoneg capability and refuse if present */
    {
        uint16_t bmsr = 0;

        ret = miiRead(SMSC95XX_PHY_ADDR, SMSC95XX_MII_BMSR, &bmsr);
        if (ret != kIOReturnSuccess) {
            Log("BMSR read failed: 0x%x", ret);
            goto fail;
        }
        if (smsc95xx_bmsr_autoneg_capable(bmsr)) {
            /* An internal-PHY 10/100BASE-TX adapter, not a T1S dongle. Leave it
             * unclaimed rather than bringing up a link we cannot negotiate. */
            Log("PHY is autoneg-capable (BMSR 0x%04x) -- not a supported T1S adapter", bmsr);
            ret = kIOReturnUnsupported;
            goto fail;
        }
    }

    /* Read MAC address from EEPROM with provenance checks */
    {
        uint8_t eeprom_sig = 0;
        uint8_t mac[SMSC95XX_MAC_LEN] = {0};
        uint32_t e2p_cmd = 0;

        /* Check if EEPROM auto-load succeeded */
        ret = readRegister(SMSC95XX_REG_E2P_CMD, &e2p_cmd);
        if (ret != kIOReturnSuccess) {
            Log("E2P_CMD read failed: 0x%x", ret);
            goto fail;
        }
        eeprom_loaded = (e2p_cmd & SMSC95XX_E2P_LOADED) != 0;
        /* %{public}s: os_log redacts plain %s as <private>, which hid exactly the
         * provenance bit we need when diagnosing a failed EEPROM auto-load. */
        Log("E2P_CMD auto-load %{public}s", eeprom_loaded ? "LOADED" : "NOT loaded");
        if (!eeprom_loaded) {
            /* An independent provenance signal from the signature: if the chip
             * did not finish loading its EEPROM, nothing read out of it can be
             * trusted, however plausible it looks. */
            Log("EEPROM auto-load did not complete; refusing to trust its contents");
            ret = kIOReturnNotFound;
            macRejectReason = "EEPROM auto-load did not complete";
            goto mac_rejected;
        }

        /* Read and verify EEPROM signature at offset 0 */
        ret = eepromRead(SMSC95XX_EEPROM_SIG_OFFSET, &eeprom_sig, 1);
        if (ret != kIOReturnSuccess) {
            Log("EEPROM signature read failed: 0x%x", ret);
            goto fail;
        }

        if (!smsc95xx_eeprom_sig_valid(eeprom_sig)) {
            Log("EEPROM signature mismatch: offset 0 read 0x%02x, expected 0x%02x",
                eeprom_sig, SMSC95XX_EEPROM_SIGNATURE);
            ret = kIOReturnNotFound;
            macRejectReason = "EEPROM signature mismatch";
            goto mac_rejected;
        }

        /* Signature matched, read the MAC address */
        ret = eepromRead(SMSC95XX_EEPROM_MAC_OFFSET, mac, SMSC95XX_MAC_LEN);
        if (ret != kIOReturnSuccess) {
            Log("EEPROM MAC read failed: 0x%x", ret);
            goto fail;
        }

        /* Pattern checks: reject all-zeros, all-ones, and multicast */
        /* Same predicate the probe applies, shared via common/ so the two cannot
         * drift, and unit-tested there. */
        const char *implausible = nullptr;
        switch (smsc95xx_mac_plausible(mac)) {
        case SMSC95XX_MAC_PLAUSIBLE:                              break;
        case SMSC95XX_MAC_ALL_ZEROS: implausible = "all zeros";   break;
        case SMSC95XX_MAC_ALL_ONES:  implausible = "all ones";    break;
        case SMSC95XX_MAC_MULTICAST: implausible = "multicast";   break;
        }
        if (implausible != nullptr) {
            Log("EEPROM signature matched but MAC is %{public}s: "
                "%02x:%02x:%02x:%02x:%02x:%02x", implausible,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            ret = kIOReturnNotFound;
            macRejectReason = "MAC implausible";
            goto mac_rejected;
        }

        Log("MAC %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        /* All provenance checks have passed: copy the validated MAC to ivars so it
         * can be served via getHardwareAddress() to the network stack. */
        for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
            ivars->macAddress[i] = mac[i];
        }
        ivars->macValid = true;

        goto mac_publish;  /* Skip rejection path; success path reached */
    }

mac_rejected:
    /* One of the provenance checks failed, and this is the ONE place the driver decides not
     * to bring the interface up. Each check logs its own detail; this states the decision,
     * which is the line a user actually needs -- without it the log explains what was wrong
     * with the EEPROM but never says that the interface is therefore not appearing.
     *
     * %{public}s, not %s: os_log redacts a plain %s to <private>, which would hide exactly
     * the diagnostic this line exists to provide.
     *
     * There is deliberately no fallback. Inventing a MAC defeats the whole provenance gate --
     * a stable, plausible-looking wrong value is precisely what re-read-and-compare cannot
     * catch. A compile-time override lived here during M5 to make test attaches deterministic
     * while the datapath was being built; it was removed once the datapath worked, and both
     * EEPROM paths were re-proved without it. */
    Log("refusing to bring up the interface: %{public}s",
        macRejectReason ? macRejectReason : "MAC provenance check failed");
    ret = kIOReturnNotFound;
    goto fail;

mac_publish:
    /* Publish MAC and EEPROM status to ioreg from ivars->macAddress, which by this point can
     * only hold a MAC that passed every provenance check. eeprom_loaded was set during the
     * validation sequence and reflects the real EEPROM auto-load status, which is what makes
     * ioreg's SMSC95xxEEPROMLoaded report hardware state rather than a guess. */
    {
        OSDictionary *props = OSDictionary::withCapacity(2);
        if (props != nullptr) {
            /* Publish MAC address as OSData */
            OSData *macData = OSData::withBytes(ivars->macAddress, SMSC95XX_MAC_LEN);
            if (macData != nullptr) {
                props->setObject("SMSC95xxMACAddress", macData);
                OSSafeReleaseNULL(macData);
            }

            /* Publish EEPROM loaded status as OSBoolean */
            OSBoolean *loadedBool = eeprom_loaded ? kOSBooleanTrue : kOSBooleanFalse;
            props->setObject("SMSC95xxEEPROMLoaded", loadedBool);
            /* Note: do NOT release OSBoolean singletons */

            if (SetProperties(props) != kIOReturnSuccess) {
                Log("SetProperties failed; registry properties will be absent");
            }
            OSSafeReleaseNULL(props);
        }
    }

    /* INITIALISE THE CHIP. Until this landed, the driver wrote exactly two registers --
     * E2P_CMD and MII_ADDR, both merely access helpers -- and never wrote MAC_CR, so
     * receive and transmit were never switched on in hardware. Every bulk IN transfer then
     * completed instantly with zero bytes because the chip had nothing to give.
     *
     * WHY HERE, and not earlier or later:
     *  - AFTER the MAC provenance gate above, necessarily: the sequence programs the
     *    station address into ADDRL/ADDRH, so it needs a MAC it is allowed to use.
     *    ivars->macAddress is only populated once that gate has passed, so reading it here
     *    cannot bypass or weaken the gate: a rejection has already jumped to fail, and
     *    execution does not reach this point at all.
     *  - BEFORE the pool, the queues and the datapath: those exist to move frames, and a
     *    chip that did not initialise has no business having any of them built, still less
     *    a network interface registered. The lite reset at the head of the sequence also
     *    resets the MAC, so running it after the pipes were armed would be resetting the
     *    hardware under an in-flight transfer.
     *
     * A failure fails Start() through the ordinary goto fail path. That is deliberate and
     * it is the point: an interface that looks healthy over a chip that is not receiving is
     * worse than no interface at all. */
    {
        SMSC95xxInitIO ioCtx = { this, 0, 0 };
        smsc95xx_io    io    = { &ioCtx, initRegRead, initRegWrite };

        Log("init: starting chip initialisation for MAC %02x:%02x:%02x:%02x:%02x:%02x "
            "(promiscuous=false, %u writes expected)",
            ivars->macAddress[0], ivars->macAddress[1], ivars->macAddress[2],
            ivars->macAddress[3], ivars->macAddress[4], ivars->macAddress[5],
            (unsigned int)SMSC95XX_INIT_WRITE_COUNT);

        /* promiscuous=false: this driver programs no multicast filter and refuses
         * setMulticastAddresses, but PRMS is not the way to paper over that -- it would
         * hand the stack every frame on the segment. setPromiscuousModeEnable is where
         * that belongs when it is implemented. */
        int irc = smsc95xx_init_seq(&io, ivars->macAddress, false);
        ret = initSeqResultToIOReturn(irc);
        if (ret != kIOReturnSuccess) {
            Log("init: FAILED after %u writes and %u reads: rc %d -> 0x%x -- refusing to "
                "start rather than present an interface over an uninitialised chip",
                ioCtx.writes, ioCtx.reads, irc, ret);
            goto fail;
        }
        Log("init: complete -- %u writes, %u reads. MAC_CR now carries RXEN|TXEN|RCVOWN, "
            "TX_CFG has TX_ON, HW_CFG has MEF", ioCtx.writes, ioCtx.reads);
    }

    /* The driver's OWN Default queue, not a dedicated one, and that is the whole point:
     * TxDataAvailable, RxComplete and TxComplete all carry no QUEUENAME, so DriverKit
     * delivers them on the queue DriverKit created for this IOService in init() under the
     * name kIOServiceDefaultQueueName. Handing that same queue to the four packet queues
     * below is what makes "everything serialises, so no locking" true by construction. A
     * dedicated queue -- which is what this used to create -- split the datapath across two
     * queues that share one packet pool: our callbacks on Default, the family's work on the
     * other one. That is a race waiting for traffic, not a design.
     * CopyDispatchQueue returns a retained queue ("the caller should release this queue"),
     * so TearDown's existing release stays correct. */
    ret = CopyDispatchQueue(kIOServiceDefaultQueueName, &ivars->dispatchQueue);
    if (ret != kIOReturnSuccess || ivars->dispatchQueue == nullptr) {
        Log("CopyDispatchQueue(Default) failed: 0x%x", ret);
        ret = (ret == kIOReturnSuccess) ? kIOReturnNoResources : ret;
        goto fail;
    }

    {
        /* 64 packets of 2048 bytes: comfortably above the 1514-byte maximum frame
         * plus the 8-byte TX command header and the 4-byte RX status word, on a
         * 10 Mb/s half-duplex segment where depth buys nothing. */
        IOUserNetworkPacketBufferPoolOptions options = {};
        options.packetCount         = 64;
        options.bufferCount         = 64;
        options.bufferSize          = 2048;
        options.maxBuffersPerPacket = 1;

        /* poolFlags was left at zero, and that PANICKED THE MACHINE.
         *
         * PoolFlagMapToDext is documented as "the pool memory will be mapped to driver
         * process memory space". Without it the pool is never mapped here, and
         * IOUserNetworkPacket::getDataVirtualAddress() hands back something that is not a
         * usable address in this process. The RX path memcpy'd a received frame straight
         * to it and took EXC_BAD_ACCESS inside _platform_memmove:
         *
         *   KERN_INVALID_ADDRESS at 0xa800 / 0xc000 / 0xd800
         *     _platform_memmove
         *     SMSC95xxDriver::RxComplete_Impl
         *
         * Those faulting addresses are the giveaway: this pool is 64 x 2048 = 131072
         * bytes, and 0xa800, 0xc000 and 0xd800 are 43008, 49152 and 55296 -- exactly
         * packet 21, 24 and 27 at 2048 bytes each. They were pool-relative offsets being
         * used as addresses.
         *
         * The cost of that was not one crash. The driver died on every received frame,
         * IOKit relaunched it through xpcproxy each time, and the respawn loop exhausted
         * SPTM shared regions and panicked the kernel:
         *   panic ... [SPTM] VIOLATION_SHARED_REGIONS_EXHAUSTED: shared_region_alloc
         *
         * Direction flags are relative to the DEVICE, so a pool serving both directions
         * needs both: the device reads frames we transmit and writes frames we receive. */
        options.poolFlags = PoolFlagMapToDext
                          | PoolFlagIODirectionIn
                          | PoolFlagIODirectionOut;

        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(this, "SMSC95xxPool",
                                                              &options, &ivars->pool);
        if (ret != kIOReturnSuccess) {
            Log("packet buffer pool creation failed: 0x%x", ret);
            goto fail;
        }
    }

    /* All four queues are built the same way, with our own dispatch queue: Create is what
     * gives a queue its dsQueue and the shared-memory data queue behind it, and that data
     * queue is the TX doorbell setupDatapath() then attaches a handler to. The withPool
     * overload would have taken a DequeueAction callback directly but takes no
     * IODispatchQueue at all, and no shipping Apple network dext uses it.
     *
     * The queue IDs are DISTINCT per queue role, not all zero as they were until now.
     * Read off a working driver: the IOReport legend AppleBCMWLAN publishes names its
     * queues "TX Submission ... Queue ID 0", "TX Completion ... Queue ID 4",
     * "RX Submission ... Queue ID 0" and "RX Completion ... Queue ID 5", so submission
     * queues share id 0 across the two directions while the completion queues take 4 and
     * 5. This driver passed 0 for all four.
     *
     * Under test: setEnable(true) refuses exactly txSubmit and rxComplete with
     * kIOReturnNotReady while rxSubmit and txComplete accept, and the stack never enqueues
     * a transmit packet. Colliding ids are one explanation for two of four queues being
     * unusable. This is a hypothesis, not a known fix -- if setEnable still refuses the
     * same two queues, ids were not the problem and the remaining lead is that the driver
     * never dequeues rxSubmit at all. */
    ret = IOUserNetworkTxSubmissionQueue::Create(ivars->pool, this,
                                                SMSC95XX_TX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_TX_SUBMIT,
                                                ivars->dispatchQueue, &ivars->txSubmit);
    if (ret != kIOReturnSuccess) { Log("TX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkTxCompletionQueue::Create(ivars->pool, this,
                                                SMSC95XX_TX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_TX_COMPLETE,
                                                ivars->dispatchQueue, &ivars->txComplete);
    if (ret != kIOReturnSuccess) { Log("TX completion queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxSubmissionQueue::Create(ivars->pool, this,
                                                SMSC95XX_TX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_RX_SUBMIT,
                                                ivars->dispatchQueue, &ivars->rxSubmit);
    if (ret != kIOReturnSuccess) { Log("RX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxCompletionQueue::Create(ivars->pool, this,
                                                SMSC95XX_TX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_RX_COMPLETE,
                                                ivars->dispatchQueue, &ivars->rxComplete);
    if (ret != kIOReturnSuccess) { Log("RX completion queue failed: 0x%x", ret); goto fail; }

    Log("pool and four queues created (queue ids tx-sub %u, tx-cmp %u, rx-sub %u, rx-cmp %u)",
        SMSC95XX_QID_TX_SUBMIT, SMSC95XX_QID_TX_COMPLETE,
        SMSC95XX_QID_RX_SUBMIT, SMSC95XX_QID_RX_COMPLETE);

    /* Bulk pipes, transfer buffers and the two completion actions. Acquired after the
     * pool and the queues so that TearDown's order -- datapath, queues, pool -- is the
     * exact reverse of setup. No RX read is armed here: that is Task 7's job, and the
     * frame paths land with it. */
    ret = setupDatapath();
    if (ret != kIOReturnSuccess) {
        Log("datapath setup failed: 0x%x", ret);
        goto fail;
    }

    {
        /* The NDK_24 overload, which takes the MAC, NOT the NDK_22 one that does not.
         *
         * Measured reason: with the no-MAC overload the family's own interface descriptor
         * is never populated, and the instrumented build showed
         * `getHardwareAddress: super -> 00:00:00:00:00:00`. Serving the real address from
         * our getHardwareAddress() override is not enough if any part of Skywalk reads the
         * descriptor instead of calling the getter -- the netif would then be configured
         * with an all-zero hardware address, which is a plausible reason for a netif to
         * refuse to transmit. Apple's own AppleUserECM dext passes the MAC in.
         *
         * One pool serves both directions, which the header explicitly permits.
         * Submission queues first, then completion. */
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->txSubmit, ivars->rxSubmit, ivars->txComplete, ivars->rxComplete
        };

        ether_addr_t hwAddr = {};
        memcpy(hwAddr.octet, ivars->macAddress, SMSC95XX_MAC_LEN);
        Diag("registerEthernetInterface: passing MAC %02x:%02x:%02x:%02x:%02x:%02x "
             "via the NDK_24 overload so the family descriptor is populated",
             hwAddr.octet[0], hwAddr.octet[1], hwAddr.octet[2],
             hwAddr.octet[3], hwAddr.octet[4], hwAddr.octet[5]);

        ret = registerEthernetInterface(hwAddr, queues, 4, ivars->pool, ivars->pool);
        if (ret != kIOReturnSuccess) {
            Log("registerEthernetInterface failed: 0x%x", ret);
            goto fail;
        }
        Log("ethernet interface registered");
        Diag("registerEthernetInterface returned 0x%x -- everything after this point is "
             "the family driving us", ret);

        /* The queues are NOT enabled here. They are enabled from
         * setInterfaceEnable(true), once the interface itself has been enabled.
         *
         * Measured: enabling them at this point has txSubmit and rxComplete refuse with
         * kIOReturnNotReady, while rxSubmit and txComplete accept. Two of four queues
         * therefore stayed disabled, and the stack never enqueued a single transmit
         * packet -- Opkts 0 with a correct route and ARP stuck at (incomplete). Moving
         * the calls into setInterfaceEnable turns both refusals into success and TX
         * starts immediately.
         *
         * This is easy to miss because a pre-existing network service enables the
         * interface almost as soon as it is registered, which makes Start() look like a
         * valid place to do it. With no service present the interface stays down --
         * ifconfig shows neither UP nor RUNNING, active media "none" -- and
         * kIOReturnNotReady means exactly what it says. */
    }

    /* Report active while the dongle is attached. A T1S multidrop segment without
     * PLCA carries no continuous link-integrity signal, so there is nothing truthful
     * to poll: BMSR bit 2 reads set with the cable unplugged (measured at M2) and must
     * never be presented as link truth. 10BASE-T is the closest available media word --
     * there is no 10BASE-T1S constant -- and the link is genuinely half duplex. */
    ret = reportLinkStatus(kIOUserNetworkLinkStatusActive,
                           SMSC95XX_MEDIA_WORD);
    if (ret != kIOReturnSuccess) {
        Log("reportLinkStatus failed: 0x%x", ret);
        goto fail;
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        Log("RegisterService failed: 0x%x", ret);
        goto fail;
    }
    return kIOReturnSuccess;

fail:
    ::TearDown(this, provider);
    Stop(provider, SUPERDISPATCH);
    return ret;
}

kern_return_t
IMPL(SMSC95xxDriver, Stop)
{
    Log("Stop");
    Diag("Stop(provider=0x%llx) entered", DiagPtr(provider));
    ::TearDown(this, provider);
    return Stop(provider, SUPERDISPATCH);
}

/* There are TWO of these and they are easy to confuse. The capital-S
 * SetInterfaceEnable is the deprecated form (iig:114); the lowercase setInterfaceEnable
 * (iig:205, LOCALONLY NDK_21) is the live one, and it is the one observed being called on
 * `ifconfig up`/`down`. This stub returns success so it cannot be the thing that blocks a
 * bring-up, but its return value has never been seen to matter. */
kern_return_t
SMSC95xxDriver::SetInterfaceEnable_Impl(bool isEnable __unused)
{
    return kIOReturnSuccess;
}

/* Deprecated form: exists so the class matches the SDK's declaration. The stack calls
 * the lowercase one below. */
kern_return_t
SMSC95xxDriver::SetPromiscuousModeEnable_Impl(bool enable __unused)
{
    return kIOReturnSuccess;
}

IOReturn
SMSC95xxDriver::setPromiscuousModeEnable(bool enable)
{
    /* Accepted but NOT yet acted on. Start() now writes MAC_CR -- with RXEN, TXEN and
     * RCVOWN, and with PRMS CLEAR -- so the register this would have to touch is finally
     * being programmed; what is still missing is the read-modify-write of MAC_CR.PRMS from
     * here. Returning success therefore remains a promise, and it is logged so the gap
     * stays visible rather than assumed. */
    Log("setPromiscuousModeEnable(%{public}s) -- accepted, but MAC_CR.PRMS is not toggled "
        "yet (Start() leaves it clear)", enable ? "true" : "false");
    /* DIAG5 (temporary): this override did not chain before. Chain now and log both
     * returns, so a mismatch between our unconditional success and the family's own result
     * shows up in the trace. Our return value is deliberately unchanged. */
    IOReturn superRet = super::setPromiscuousModeEnable(enable);
    Diag("setPromiscuousModeEnable(%{public}s): super -> 0x%x; ours -> 0x%x",
         enable ? "true" : "false", superRet, kIOReturnSuccess);
    return kIOReturnSuccess;
}

/* The live MTU pair. v1 carries exactly one MTU: the pool buffers are 2048 bytes and
 * there is no scatter-gather, so anything larger cannot be transmitted. */
IOReturn
SMSC95xxDriver::setMaxTransferUnit(uint32_t mtu)
{
    /* DIAG5 (temporary): this override did not chain before, and it now chains
     * UNCONDITIONALLY -- including on the path where we go on to refuse the MTU. That is a
     * real, if small, behaviour change for this diagnostic build only: the family's own
     * setMaxTransferUnit runs even for an MTU we then reject. It is accepted here because
     * the refusal path is not expected to be exercised (the stack asks for 1500) and
     * because seeing the family's result on both paths is the point. Our return value is
     * unchanged. */
    IOReturn superRet = super::setMaxTransferUnit(mtu);
    IOReturn ours     = (mtu == 1500) ? kIOReturnSuccess : kIOReturnUnsupported;
    Diag("setMaxTransferUnit(%u): super -> 0x%x; ours -> 0x%x%{public}s",
         mtu, superRet, ours,
         (mtu != 1500) ? "  (refused: only 1500 is supported)" : "");
    if (mtu != 1500) {
        Log("setMaxTransferUnit(%u) refused -- only 1500 is supported", mtu);
    }
    return ours;
}

uint32_t
SMSC95xxDriver::getMaxTransferUnit()
{
    /* DIAG5 (temporary): chain purely to observe the family's default; we still return
     * our own 1500, because the 2048-byte pool buffers are what actually bound this. */
    uint32_t superMtu = super::getMaxTransferUnit();
    Diag("getMaxTransferUnit(): super -> %u; ours -> 1500", superMtu);
    return 1500;
}

IOReturn
SMSC95xxDriver::setInterfaceEnable(bool enable)
{
    Log("setInterfaceEnable(%{public}s)", enable ? "true" : "false");

    /* Chain to the base class FIRST, and do not swallow its result.
     *
     * Omitting this was a latent bug from M4 that only surfaced once the datapath needed
     * to move frames. IOUserNetworkEthernet::setInterfaceEnable is what puts the packet
     * queues into a state where the stack will enqueue to them; an override that reports
     * link status and returns success leaves the interface looking perfectly healthy --
     * UP, RUNNING, correct media, status active -- while the stack never hands it a single
     * packet. Measured symptom: a correct route to the interface, an ARP entry stuck at
     * (incomplete), Opkts 0, and the TX doorbell never firing once.
     *
     * super:: rather than SUPERDISPATCH because setInterfaceEnable is LOCALONLY
     * (IOUserNetworkEthernet.iig:205), so it is an ordinary C++ virtual call within the
     * driver process rather than a dispatched one. */
    IOReturn ret = super::setInterfaceEnable(enable);
    /* DIAG5 (temporary): this one already chained; the extra line only puts it in the
     * same selectable trace as the rest. */
    Diag("setInterfaceEnable(%{public}s): super -> 0x%x; ours -> 0x%x unless super failed",
         enable ? "true" : "false", ret, kIOReturnSuccess);
    if (ret != kIOReturnSuccess) {
        Log("super::setInterfaceEnable(%{public}s) failed: 0x%x",
            enable ? "true" : "false", ret);
        return ret;
    }

    if (enable) {
        /* Enable the four packet queues HERE, after super has enabled the interface, and
         * NOT at registration time in Start().
         *
         * Measured, and the reason the stack would not transmit at all: called from
         * Start(), txSubmit and rxComplete refuse with kIOReturnNotReady while rxSubmit
         * and txComplete accept, so two of four queues stay disabled and Opkts never
         * leaves 0. Called from here, all four succeed and transmit begins immediately.
         *
         * setEnable, not SetEnable: the capital form is the deprecated LOCAL one. */
        static const char *const queueNames[4] = {
            "tx submission", "rx submission", "tx completion", "rx completion"
        };
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->txSubmit, ivars->rxSubmit, ivars->txComplete, ivars->rxComplete
        };
        for (uint32_t i = 0; i < 4; i++) {
            if (queues[i] == nullptr) {
                continue;
            }
            IOReturn er = queues[i]->setEnable(true);
            if (er != kIOReturnSuccess) {
                Log("setEnable(true) failed on the %{public}s queue: 0x%x",
                    queueNames[i], er);
                return er;
            }
        }
        Log("all four packet queues enabled");

        /* Report the link active when the stack brings the interface up, not just at
         * registration time. A link status reported during Start() is discarded because
         * the interface is not yet enabled; this call happens when the user (or the
         * system) actually enables it via ifconfig or similar. It has to come AFTER the
         * super call for the same reason -- the interface must be enabled first. */
        ret = reportLinkStatus(kIOUserNetworkLinkStatusActive,
                               kIOUserNetworkMediaEthernet10BaseT |
                               kIOUserNetworkMediaOptionHalfDuplex);
        if (ret != kIOReturnSuccess) {
            Log("reportLinkStatus in setInterfaceEnable failed: 0x%x", ret);
            return ret;
        }

        /* Arm the first bulk IN read HERE, and not in Start().
         *
         * Two reasons, one certain and one being tested. The certain one: a completion that
         * arrives before the stack is ready has nowhere to deliver frames -- the RX
         * completion queue answers kIOReturnNotReady until the interface is enabled, so every
         * frame in that first transfer would be allocated, copied, refused and thrown away.
         * By this point super::setInterfaceEnable has returned success and the link has been
         * reported active, so the queue is as ready as the family will make it.
         *
         * The one being tested: nothing has ever armed this read, so the datapath has only
         * ever been half alive, and the two enqueue paths that fail with kIOReturnNotReady
         * (txSubmit and rxComplete) are exactly the two halves of a complete loop. The
         * hypothesis is that Skywalk does not start the datapath until both directions are
         * live, which would make this call the thing that unblocks TX. It is a hypothesis --
         * if TxDataAvailable still never fires with RX armed and receiving, it is wrong, and
         * the DIAG7 trace is what says which.
         *
         * A failure here fails the enable rather than leaving a half-open interface: an
         * interface the stack believes is up but that cannot receive is worse than one that
         * refused to come up. */
        /* Set BEFORE arming, not after: with everything on one dispatch queue the completion
         * cannot run until this returns, but ordering the state change first means the
         * invariant "a read is only outstanding while rxRunning" is never momentarily false. */
        ivars->rxRunning = true;
        /* Start each enable from a clean slate: a run of empty completions from a previous
         * session must not push a freshly enabled interface straight into backoff. */
        ivars->rxIdleRun       = 0;
        ivars->rxBackoffActive = false;
        ret = armRxRead();
        Diag("setInterfaceEnable(true): armRxRead -> 0x%x", ret);
        if (ret != kIOReturnSuccess) {
            Log("setInterfaceEnable: failed to arm the first RX read: 0x%x", ret);
            ivars->rxRunning = false;
            return ret;
        }
        Log("setInterfaceEnable: RX armed");
    } else {
        /* Let the receive loop wind down. The outstanding transfer is deliberately NOT
         * aborted -- that belongs to teardown, and aborting a pipe the interface may be about
         * to re-enable buys nothing -- so one more completion can still arrive; RxComplete
         * sees rxRunning false, discards it and does not re-arm. */
        ivars->rxRunning = false;
        Log("setInterfaceEnable(false): RX will stop after the outstanding read completes");
        /* A run of traffic has just ended, and this is the earlier of the two moments where
         * totals are worth having -- an interface that is taken down and brought back up
         * without the dext restarting would otherwise only ever be summarised at teardown,
         * with the two runs added together. */
        logDatapathCounters("interface disable");
    }
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::SetMulticastAddresses_Impl(const IOUserNetworkMACAddress *addresses __unused,
                                           uint32_t count __unused)
{
    /* v1 has no multicast filtering, and refusing is now honest: Start() writes MAC_CR with
     * PRMS clear and HASHH/HASHL zeroed, so the hardware really is filtering to the station
     * address and broadcast rather than passing everything. Real hash filtering is the
     * follow-on work. */
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SetAllMulticastModeEnable_Impl(bool enable __unused)
{
    /* v1 has no multicast filtering, and refusing is now honest for the same reason as
     * SetMulticastAddresses_Impl above: Start() programs MAC_CR and the hash registers, so
     * the hardware filter is in a known state rather than an unwritten one. */
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SelectMediaType_Impl(IOUserNetworkMediaType mediaType __unused)
{
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SetWakeOnMagicPacketEnable_Impl(bool enable __unused)
{
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SetMTU_Impl(uint32_t mtu)
{
    /* Accept only the standard MTU; claiming success for anything else would let
     * the stack send frames the hardware path cannot carry. */
    return (mtu == 1500) ? kIOReturnSuccess : kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::GetMaxTransferUnit_Impl(uint32_t *mtu)
{
    if (mtu == nullptr) {
        return kIOReturnBadArgument;
    }
    *mtu = 1500;                 /* v1 has no jumbo support */
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::SetHardwareAssists_Impl(uint32_t hardwareAssists __unused)
{
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::GetHardwareAssists_Impl(uint32_t *hardwareAssists)
{
    if (hardwareAssists == nullptr) {
        return kIOReturnBadArgument;
    }
    *hardwareAssists = 0;        /* no checksum offload, no TSO, no VLAN -- v1 scope */
    return kIOReturnSuccess;
}

/* Verified against IOUserNetworkEthernet.iig:309 --
 *   getHardwareAddress(ether_addr_t *addr) LOCALONLY NDK_21
 * ether_addr_t is 6 bytes (octet[6]); it is not the same type as the deprecated
 * IOUserNetworkMACAddress used by the older RegisterEthernetInterface overload. */
kern_return_t
SMSC95xxDriver::getHardwareAddress(ether_addr_t *addr)
{
    /* DIAG5 (temporary). The family's own default is read into a LOCAL buffer and only
     * logged: it is never copied into the caller's `addr` and never returned. The MAC
     * provenance gate below -- refuse unless ivars->macValid -- is untouched, unreordered
     * and still the only thing that decides what the network stack sees. */
    {
        ether_addr_t  superAddr = {};
        kern_return_t superRet  = super::getHardwareAddress(&superAddr);
        Diag("getHardwareAddress: super -> 0x%x %02x:%02x:%02x:%02x:%02x:%02x; "
             "ours -> 0x%x %02x:%02x:%02x:%02x:%02x:%02x (macValid=%{public}s)",
             superRet,
             superAddr.octet[0], superAddr.octet[1], superAddr.octet[2],
             superAddr.octet[3], superAddr.octet[4], superAddr.octet[5],
             ivars->macValid ? kIOReturnSuccess : kIOReturnNotReady,
             ivars->macAddress[0], ivars->macAddress[1], ivars->macAddress[2],
             ivars->macAddress[3], ivars->macAddress[4], ivars->macAddress[5],
             ivars->macValid ? "true" : "false");
    }

    if (addr == nullptr) {
        return kIOReturnBadArgument;
    }
    if (!ivars->macValid) {
        /* Never hand back a zero or synthesised address: refusing is what keeps a
         * mis-clocked EEPROM read from reaching the network stack. */
        Log("getHardwareAddress before a validated MAC was available");
        return kIOReturnNotReady;
    }
    /* A loop, not memcpy: the dext currently pulls in no string.h, and there is no
     * reason to add a platform header for six bytes. */
    for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
        addr->octet[i] = ivars->macAddress[i];
    }
    return kIOReturnSuccess;
}

/* IOUserNetworkEthernet.iig:185 --
 *   getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount) LOCALONLY NDK_21
 * One entry only: 10BASE-T half duplex. BMSR bit 3 is clear on both dongles, so there
 * is no autonegotiation and nothing else honest to offer. */
/* DIAG5 (temporary): capacity of the scratch array the family's own default is asked to
 * fill. _ReportAvailableMediaTypes carries up to 256 words, so 64 is well clear of any
 * plausible default list while keeping the stack frame small. */
#define SMSC95XX_DIAG_MEDIA_MAX 64

IOReturn
SMSC95xxDriver::getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount)
{
    /* DIAG5 (temporary): chain into a LOCAL array, never the caller's, so the single
     * entry we advertise still reaches the family unchanged. */
    {
        MediaWord superArray[SMSC95XX_DIAG_MEDIA_MAX] = {};
        uint32_t  superCount = SMSC95XX_DIAG_MEDIA_MAX;
        IOReturn  superRet   = super::getSupportedMediaArray(superArray, &superCount);
        MediaWord superFirst = (superCount > 0 && superCount <= SMSC95XX_DIAG_MEDIA_MAX)
                             ? superArray[0] : 0u;
        Diag("getSupportedMediaArray: super -> 0x%x, count %u, first 0x%08x; "
             "ours -> 0x%x, count 1, entry 0x%08x",
             superRet, superCount, superFirst,
             (mediaCount == nullptr) ? kIOReturnBadArgument : kIOReturnSuccess,
             (uint32_t)SMSC95XX_MEDIA_WORD);
    }

    if (mediaCount == nullptr) {
        return kIOReturnBadArgument;
    }

    /* Three entries: none, autoselect, then the one real medium. Advertising only the
     * real medium -- which is what this did until now -- is inconsistent in a way that
     * is visible in two places:
     *
     *   - System Settings > Hardware leaves BOTH the Speed and Duplex dropdowns EMPTY,
     *     because it builds them from this list and a single entry gives it nothing to
     *     choose between.
     *   - the family's own initial media is kIOUserNetworkMediaEthernetAuto: the
     *     instrumented build showed `getInitialMedia(): super -> 0x00000020`, which is
     *     IFM_ETHER|IFM_AUTO. So the interface starts in a mode we did not claim to
     *     support, and `ifconfig` duly reports `media: autoselect (...)` -- a current
     *     media absent from our own supported list.
     *
     * Every working adapter on this machine offers none and autoselect first; the
     * Belkin USB-C LAN lists seven entries beginning with exactly those two. Matching
     * that shape is why the order here is none, auto, specific.
     *
     * mediaCount is an OUTPUT, not a capacity: the stack calls with *mediaCount == 0 and
     * a non-null array, and treating it as capacity produced SIOCGIFXMEDIA: Input/output
     * error at M4. The caller's array is sized for a normal media list -- seven entries
     * for the adapter above -- so three is well within it. */
    if (mediaArray != nullptr) {
        mediaArray[0] = kIOUserNetworkMediaEthernetNone;
        mediaArray[1] = kIOUserNetworkMediaEthernetAuto;
        mediaArray[2] = SMSC95XX_MEDIA_WORD;
        *mediaCount   = 3;
        return kIOReturnSuccess;
    }

    /* If mediaArray is null (should not happen), report the count anyway for safety. */
    *mediaCount = 3;
    return kIOReturnSuccess;
}

/* Return 0 to prevent the stack from using offloads we do not implement.
 * The deprecated capital-G GetHardwareAssists_Impl is not called by the stack;
 * this is the live method per IOUserNetworkEthernet.iig:228 (LOCALONLY NDK_21). */
uint32_t
SMSC95xxDriver::getHardwareAssists()
{
    /* DIAG5 (temporary): this override did not chain before. The family's default is the
     * prime suspect for the TSO4/TSO6/PARTIAL_CSUM that ifconfig shows despite this
     * returning 0, so log both. Our return value is unchanged. */
    uint32_t superAssists = super::getHardwareAssists();
    Diag("getHardwareAssists(): super -> 0x%08x; ours -> 0x00000000 (no offloads)",
         superAssists);
    Log("getHardwareAssists: returning 0 (no offloads)");
    return 0;
}

/* Experiment: test whether explicit TSO options suppress the offload flags.
 * Per IOUserNetworkEthernet.iig:375 (LOCALONLY NDK_22). Returns a zero-initialised
 * struct, which should advertise that we support no TSO. */
IOReturn
SMSC95xxDriver::getTSOOptions(IOUserNetworkTSOOptions *options)
{
    /* DIAG5 (temporary): this override did not chain before. Chain into a LOCAL struct so
     * the caller's buffer is still zeroed by us, and log what the family would have
     * offered -- a non-zero mss4/mss6 here would explain the TSO advertisement. */
    IOUserNetworkTSOOptions superOptions = {};
    IOReturn superRet = super::getTSOOptions(&superOptions);

    if (options == nullptr) {
        Diag("getTSOOptions(nullptr): super -> 0x%x mss4 %u mss6 %u; ours -> 0x%x",
             superRet, superOptions.mss4, superOptions.mss6, kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
    /* Zero the WHOLE struct: it is a union of {tso_mtu4, tso_mtu6} plus a 56-byte
     * reserved tail, so assigning the two named fields would leave 56 caller-provided
     * bytes untouched while claiming to be zero-initialised. */
    *options = IOUserNetworkTSOOptions{};
    Diag("getTSOOptions: super -> 0x%x mss4 %u mss6 %u; ours -> 0x%x mss4 0 mss6 0",
         superRet, superOptions.mss4, superOptions.mss6, kIOReturnSuccess);
    return kIOReturnSuccess;
}

/* Diagnostic: log what assist capabilities the stack requests, and reject them.
 * Per IOUserNetworkEthernet.iig:369 (LOCALONLY NDK_22). The requested bitmask
 * alone tells us what the family thinks it should enable, which is useful for
 * M5's integration test. We support none of them. */
IOReturn
SMSC95xxDriver::setHardwareAssists(uint32_t hardwareAssists, uint32_t hardwareAssistsMask)
{
    Log("setHardwareAssists: requested=0x%08x, mask=0x%08x", hardwareAssists, hardwareAssistsMask);
    /* DIAG5 (temporary): this override did not chain before. Refusing an assist the family
     * expects to be able to set is a candidate cause in itself, so log what it would have
     * returned. Our return value is unchanged. */
    IOReturn superRet = super::setHardwareAssists(hardwareAssists, hardwareAssistsMask);
    Diag("setHardwareAssists(requested=0x%08x, mask=0x%08x): super -> 0x%x; ours -> 0x%x",
         hardwareAssists, hardwareAssistsMask, superRet, kIOReturnUnsupported);
    return kIOReturnUnsupported;
}

/* =====================================================================================
 * TEMPORARY M5 DIAGNOSTIC INSTRUMENTATION -- REVERT THIS WHOLE BLOCK, together with the
 * matching declarations in SMSC95xxDriver.iig and the DIAG5 lines added to the overrides
 * above.
 *
 * Purpose: find out what NetworkingDriverKit actually asks of this driver between
 * registerEthernetInterface() and the first TX enqueue. The interface comes up (UP,
 * RUNNING, correct MAC, status active) and the route resolves to it, yet the stack never
 * enqueues a packet and TxDataAvailable has never fired once. AppleUserECM -- Apple's own
 * USB Ethernet dext -- overrides or calls every method below, and this driver overrode
 * none of them, so any one of them could be the gate that is being answered by a default
 * that does not fit us.
 *
 * Every function here is log-and-chain: it records that it was called, with its arguments
 * and the value being returned, and then returns IOUserNetworkEthernet's own result
 * unmodified. Behaviour is therefore identical to not overriding them at all. All of these
 * are LOCALONLY in IOUserNetworkEthernet.iig, so `super::` -- an ordinary in-process C++
 * virtual call -- is the correct chain; SUPERDISPATCH would be wrong. Each base
 * implementation was confirmed to be an exported symbol in NetworkingDriverKit.tbd before
 * being chained to, so none of these is a link-time guess.
 * ===================================================================================== */

/* iig:176. The lowercase LOCALONLY NDK_21 hook, NOT IOService::Start. It is reached from
 * inside IOUserNetworkEthernet's own Start implementation, i.e. during the
 * Start(provider, SUPERDISPATCH) call at the top of our capital-S Start -- so this runs
 * BEFORE any of our setup, and chaining to super cannot reorder it. */
bool
SMSC95xxDriver::start(IOService *provider)
{
    Diag("start(provider=0x%llx) entered [lowercase NDK_21 hook]", DiagPtr(provider));
    bool ok = super::start(provider);
    Diag("start(provider=0x%llx): super -> %{public}s", DiagPtr(provider),
         ok ? "true" : "false");
    return ok;
}

/* iig:179. Counterpart of the above, reached from inside IOUserNetworkEthernet's Stop.
 * Our own teardown is driven from capital-S Stop and is not touched. */
void
SMSC95xxDriver::stop(IOService *provider)
{
    Diag("stop(provider=0x%llx) entered [lowercase NDK_21 hook]", DiagPtr(provider));
    super::stop(provider);
    Diag("stop(provider=0x%llx): returned from super", DiagPtr(provider));
}

/* iig:182. This driver does no power management of its own; the value is interesting only
 * because a power-state transition the family is waiting on would show up here. */
IOReturn
SMSC95xxDriver::setPowerState(unsigned long powerState, IOService *whatDevice)
{
    IOReturn ret = super::setPowerState(powerState, whatDevice);
    Diag("setPowerState(powerState=%lu, whatDevice=0x%llx): super -> 0x%x",
         powerState, DiagPtr(whatDevice), ret);
    return ret;
}

/* iig:128. The LIVE lowercase counterpart of the deprecated pure-virtual
 * SetMulticastAddresses this driver already stubs. The deprecated one returns
 * kIOReturnUnsupported and is not the one the stack calls; this is. */
IOReturn
SMSC95xxDriver::setMulticastAddresses(const ether_addr_t *addresses, uint32_t count)
{
    IOReturn ret = super::setMulticastAddresses(addresses, count);
    Diag("setMulticastAddresses(addresses=0x%llx, count=%u): super -> 0x%x "
         "[deprecated capital-S form of this returns 0x%x]",
         DiagPtr(addresses), count, ret, kIOReturnUnsupported);
    return ret;
}

/* iig:207. Reports the BSD name the family assigned. Purely informational, but it ties the
 * trace to the interface `ifconfig` shows. const, so it touches no ivars. */
const char *
SMSC95xxDriver::getBSDName() const
{
    const char *name = super::getBSDName();
    Diag("getBSDName(): super -> \"%{public}s\"", name != nullptr ? name : "(null)");
    return name;
}

/* iig:213. The LIVE lowercase counterpart of the deprecated pure-virtual
 * SetAllMulticastModeEnable this driver already stubs with kIOReturnUnsupported. */
IOReturn
SMSC95xxDriver::setAllMulticastModeEnable(bool enable)
{
    IOReturn ret = super::setAllMulticastModeEnable(enable);
    Diag("setAllMulticastModeEnable(%{public}s): super -> 0x%x "
         "[deprecated capital-S form of this returns 0x%x]",
         enable ? "true" : "false", ret, kIOReturnUnsupported);
    return ret;
}

/* iig:216. The LIVE replacement for the deprecated pure-virtual SelectMediaType this
 * driver stubs with kIOReturnUnsupported. If the family picks a media word we do not
 * advertise, that mismatch would appear here. */
IOReturn
SMSC95xxDriver::handleChosenMedia(MediaWord chosenMedia)
{
    IOReturn ret = super::handleChosenMedia(chosenMedia);
    Diag("handleChosenMedia(chosenMedia=0x%08x): super -> 0x%x "
         "(we advertise only 0x%08x; deprecated SelectMediaType returns 0x%x)",
         chosenMedia, ret, (uint32_t)SMSC95XX_MEDIA_WORD, kIOReturnUnsupported);
    return ret;
}

/* iig:233. `struct ifdrv` is only forward-declared in the DriverKit SDK
 * (IOUserNetworkTypes.h:188 does `typedef struct ifdrv ifdrv_t;` and nothing defines it),
 * so the command cannot be decoded without redeclaring a kernel struct by hand. Log the
 * pointer: the fact that the family issues an interface command at all, and when, is the
 * signal being looked for here. */
IOReturn
SMSC95xxDriver::processInterfaceCommand(ifdrv_t *cmd_data)
{
    IOReturn ret = super::processInterfaceCommand(cmd_data);
    Diag("processInterfaceCommand(cmd_data=0x%llx): super -> 0x%x "
         "(struct ifdrv is opaque in the DriverKit SDK, so the payload is not decoded)",
         DiagPtr(cmd_data), ret);
    return ret;
}

/* iig:262. Returns int, not IOReturn -- the header documents it as the BPF tap-state
 * callback whose non-zero return fails the attach. */
int
SMSC95xxDriver::bpfTap(uint32_t dataLinkType, uint32_t mode)
{
    int ret = super::bpfTap(dataLinkType, mode);
    Diag("bpfTap(dataLinkType=%u, mode=0x%x): super -> %d", dataLinkType, mode, ret);
    return ret;
}

/* iig:321. Chaining here does NOT weaken the MAC provenance gate: this writes nothing to
 * ivars->macAddress and does not set ivars->macValid, so getHardwareAddress() keeps
 * serving the validated EEPROM MAC exactly as before. */
IOReturn
SMSC95xxDriver::setHardwareAddress(ether_addr_t *addr)
{
    if (addr != nullptr) {
        Diag("setHardwareAddress(%02x:%02x:%02x:%02x:%02x:%02x) requested",
             addr->octet[0], addr->octet[1], addr->octet[2],
             addr->octet[3], addr->octet[4], addr->octet[5]);
    } else {
        Diag("setHardwareAddress(nullptr) requested");
    }
    IOReturn ret = super::setHardwareAddress(addr);
    Diag("setHardwareAddress: super -> 0x%x (our EEPROM MAC "
         "%02x:%02x:%02x:%02x:%02x:%02x is unchanged, macValid=%{public}s)", ret,
         ivars->macAddress[0], ivars->macAddress[1], ivars->macAddress[2],
         ivars->macAddress[3], ivars->macAddress[4], ivars->macAddress[5],
         ivars->macValid ? "true" : "false");
    return ret;
}

/* iig:324. Goes into _IOUserNetworkEthernetInterfaceDescriptor::featureFlags at
 * registration time, so an unexpected default here is a candidate gate. */
uint32_t
SMSC95xxDriver::getFeatureFlags()
{
    uint32_t flags = super::getFeatureFlags();
    Diag("getFeatureFlags(): super -> 0x%08x", flags);
    return flags;
}

/* iig:331. The interface sub-family the family registers us under. */
uint32_t
SMSC95xxDriver::getInterfaceSubFamily()
{
    uint32_t subFamily = super::getInterfaceSubFamily();
    Diag("getInterfaceSubFamily(): super -> %u (0x%08x)", subFamily, subFamily);
    return subFamily;
}

/* iig:338. Read at init time to build the BSD name. */
const char *
SMSC95xxDriver::getBSDNamePrefix()
{
    const char *prefix = super::getBSDNamePrefix();
    Diag("getBSDNamePrefix(): super -> \"%{public}s\"",
         prefix != nullptr ? prefix : "(null)");
    return prefix;
}

/* iig:345. Read at init time for the BSD unit number. */
int32_t
SMSC95xxDriver::getBSDUnitNumber()
{
    int32_t unit = super::getBSDUnitNumber();
    Diag("getBSDUnitNumber(): super -> %d", unit);
    return unit;
}

/* iig:352. The initially chosen media, which lands in the interface descriptor. If it does
 * not intersect what getSupportedMediaArray advertises, that is worth knowing. */
MediaWord
SMSC95xxDriver::getInitialMedia()
{
    MediaWord media = super::getInitialMedia();
    Diag("getInitialMedia(): super -> 0x%08x (we advertise 0x%08x)",
         media, (uint32_t)SMSC95XX_MEDIA_WORD);
    return media;
}

/* iig:360. "Return the Data offset expected for every packet given to hardware ... invoked
 * by bsd client start() when attaching a new ifnet. The default implementation returns 0."
 * This is the modern replacement for the deprecated SetTxPacketHeadroom, and it is the
 * headroom the stack leaves for our 8-byte TX command header.
 *
 * NOW ANSWERED, NOT JUST OBSERVED: the family's default is 0 and this chip needs exactly 8,
 * so returning SMSC95XX_TX_HEADER_LEN takes the family up on the offer. Both values are
 * logged, so the trace still shows what the default was.
 *
 * READ THIS BEFORE CHANGING IT TO 0. The 8 bytes are real -- getDataOff() reports 8 on every
 * TX packet -- but getDataVirtualAddress() does NOT point past them, contrary to what this
 * comment used to claim. It returns the buffer BASE; the frame starts at base + getDataOff().
 * Getting that wrong sent 8 bytes of headroom ahead of every frame and truncated it by 8, on
 * both TX and RX. Both paths now add getDataOff() explicitly, which is correct for any value
 * this function returns.
 *
 * So 8 is kept deliberately rather than reverted to the family's default of 0: a non-zero
 * offset keeps that arithmetic exercised on every frame instead of adding zero, and it is
 * what M6 needs if the zero-copy in-place transmit lands. Returning 0 here would make both
 * datapaths silently stop testing the thing that was wrong. */
uint16_t
SMSC95xxDriver::getTxDataOffset()
{
    uint16_t offset = super::getTxDataOffset();
    Diag("getTxDataOffset(): super -> %u; ours -> %u (SMSC95XX_TX_HEADER_LEN)",
         offset, (unsigned int)SMSC95XX_TX_HEADER_LEN);
    return (uint16_t)SMSC95XX_TX_HEADER_LEN;
}

/* ========================= END TEMPORARY DIAGNOSTIC BLOCK ========================== */
