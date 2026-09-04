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
#include "smsc95xx_init.h"

/* One definition, used by both reportLinkStatus call sites and getSupportedMediaArray,
 * so the advertised media and the reported active media cannot drift apart.
 * 10BASE-T is the closest available word -- there is no 10BASE-T1S constant -- and the
 * segment is genuinely half duplex. */
#define SMSC95XX_MEDIA_WORD \
    (kIOUserNetworkMediaEthernet10BaseT | kIOUserNetworkMediaOptionHalfDuplex)

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: " fmt, ##__VA_ARGS__)

/* The DIAG5 trace: what NetworkingDriverKit asks of this driver, and what we answer.
 * Compiled in only with `make TRACE=1` (see the Makefile's TRACE block). One prefix, so
 * a single predicate selects the whole trace:
 *   log stream --predicate 'eventMessage CONTAINS "DIAG5:"'
 *
 * The disabled form is `if (0) os_log(...)`, not an empty macro, so format strings and
 * arguments stay type-checked in every build. No call site may have a side effect in
 * its arguments; that is what makes discarding the evaluation safe.
 *
 * Pointers are logged as 0x%llx through (uint64_t)(uintptr_t) rather than %p: %p wants
 * a void * and -Wformat objects to anything else. */
#if SMSC95XX_TRACE
#define Diag(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG5: " fmt, ##__VA_ARGS__)
#else
#define Diag(fmt, ...) \
    do { if (0) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG5: " fmt, ##__VA_ARGS__); } while (0)
#endif
#define DiagPtr(p)     ((uint64_t)(uintptr_t)(p))

/* Poll attempts for MII and EEPROM operations */
#define POLL_ATTEMPTS 100

/* Depth of each of the four packet queues. Matches the pool's packet count, which is the
 * real ceiling on packets in flight; generous for a 10 Mb/s half-duplex segment with one
 * transfer in flight per direction. */
#define SMSC95XX_QUEUE_CAPACITY SMSC95XX_POOL_PACKET_COUNT

/* Per-role queue ids, taken from the IOReport legend a working Apple driver publishes:
 * submission queues share id 0 across the two directions, while the completion queues
 * take 4 and 5. */
#define SMSC95XX_QID_TX_SUBMIT     0
#define SMSC95XX_QID_RX_SUBMIT     0
#define SMSC95XX_QID_TX_COMPLETE   4
#define SMSC95XX_QID_RX_COMPLETE   5

/* SMSC95xxDriver_IVars is defined in SMSC95xxDriver_ivars.h, shared with
 * SMSC95xxDriver_datapath.cpp: the class spans two translation units and both need the
 * layout. */

/* Private helper for releasing resources on Start() failure or Stop(). */
static void
ReleaseResources(SMSC95xxDriver *driver, IOService *provider);

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
    /* EEPROM addresses are 9 bits wide; written so a huge `len` cannot wrap. */
    if (buf == nullptr || len > 0x200 || offset > 0x200 - len)
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
 * The sequence itself lives in common/smsc95xx_init.c, shared verbatim with the
 * userspace probe. What lives here is the adapter: two thunks that turn the shared
 * layer's (ctx, offset, value) callbacks into this class's readRegister/writeRegister,
 * plus per-access logging so a failed init is diagnosable from a single attach rather
 * than needing a USB capture.
 * ------------------------------------------------------------------------------------- */

/* Carried through the shared layer as its opaque `ctx`. The counters are only for the
 * log: they turn "a write failed" into "the eleventh write, AFC_CFG, failed". */
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
         * sequence it happened. */
        Log("init: read #%u %{public}s (0x%03x) FAILED: 0x%x", io->reads,
            smsc95xx_reg_name(offset), offset, ret);
    } else {
        Log("init: read #%u %{public}s (0x%03x) -> 0x%08x", io->reads,
            smsc95xx_reg_name(offset), offset, *value);
    }
    /* An IOReturn is 0 on success and non-zero otherwise -- exactly the smsc95xx_io
     * contract -- so it is returned unchanged and propagates back out to Start(). */
    return (int)ret;
}

static int
initRegWrite(void *ctx, uint16_t offset, uint32_t value)
{
    SMSC95xxInitIO *io = static_cast<SMSC95xxInitIO *>(ctx);
    io->writes++;

    kern_return_t ret = io->driver->writeRegister(offset, value);
    /* EVERY write with its value, unconditionally: this is what makes an init that
     * fails on real hardware readable straight out of `log stream`. */
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

/* Publish one registry property to ioreg. A null value or a failure is logged and
 * otherwise ignored: a missing ioreg property must never fail an attach. The caller
 * keeps ownership of `value`. */
static void
publishProperty(SMSC95xxDriver *driver, const char *key, OSObject *value)
{
    if (value == nullptr) {
        return;
    }
    OSDictionary *props = OSDictionary::withCapacity(1);
    if (props == nullptr) {
        return;
    }
    props->setObject(key, value);
    if (driver->SetProperties(props) != kIOReturnSuccess) {
        Log("SetProperties(%{public}s) failed; the registry property will be absent", key);
    }
    OSSafeReleaseNULL(props);
}

/* Release every resource the driver holds. Must be safe to call after any subset of ivars
 * has been populated, and is idempotent (OSSafeReleaseNULL nulls as it releases).
 *
 * MUST NOT be called while anything can still be running on the driver's dispatch queue:
 * the datapath callbacks dereference the packet queues, the pool and the RX buffer, so
 * releasing these under a live handler is exactly the use-after-free this ordering exists
 * to prevent. Callers guarantee that by having quiesced and cancelled first -- see Stop().
 *
 * The order is fixed: the datapath first, then the four queues before the pool they hold
 * references to, then the interface, then the dispatch queue last. */
static void
ReleaseResources(SMSC95xxDriver *driver, IOService *provider __unused)
{
    /* The datapath goes FIRST: its buffers are what an in-flight transfer would have
     * referenced, and its pipes must be released before the queues that outlive them. */
    driver->releaseDatapath();

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
         * null it so a second ReleaseResources cannot issue a second Close. */
        driver->ivars->interface = nullptr;
    }
    driver->ivars->ctrlBytes = nullptr;
    OSSafeReleaseNULL(driver->ivars->ctrlBuffer);

    /* Release the dispatch queue last. This drops the +1 CopyDispatchQueue handed us, not
     * the service's own reference: the Default queue was created by DriverKit in
     * IOService::init() and outlives this, which is why callbacks can still be delivered
     * on it while Stop unwinds. */
    OSSafeReleaseNULL(driver->ivars->dispatchQueue);
}

kern_return_t
IMPL(SMSC95xxDriver, Start)
{
    /* Trace anchors: everything the family asks of us during registration happens
     * between these two lines. */
    Diag("Start(provider=0x%llx) entered -- before Start(SUPERDISPATCH)", DiagPtr(provider));
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    Diag("Start: Start(SUPERDISPATCH) returned 0x%x", ret);
    if (ret != kIOReturnSuccess) {
        Log("Start(super) failed: 0x%x", ret);
        return ret;
    }

    Log("Start: matched provider");

    /* Function-scoped because the provenance checks below jump out of their block:
     * macRejectReason names which gate failed, and eeprom_loaded is read at mac_publish. */
    const char *macRejectReason = nullptr;
    bool eeprom_loaded = false;

    ivars->interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (ivars->interface == nullptr) {
        Log("provider is not an IOUSBHostInterface");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    /* The interface is the only USB object this class needs: DeviceRequest with
     * kIOUSBDeviceRequestRecipientDevice carries register, MII and EEPROM access to the
     * device, and configuration selection belongs to SMSC95xxUSBDevice. So no
     * CopyDevice and no SetConfiguration here.
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

        OSNumber *number = OSNumber::withNumber(idRev, 32);
        publishProperty(this, "SMSC95xxIDRev", number);
        OSSafeReleaseNULL(number);
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

        OSNumber *number = OSNumber::withNumber(phyid, 32);
        publishProperty(this, "SMSC95xxPHYID", number);
        OSSafeReleaseNULL(number);
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
        Log("E2P_CMD auto-load %{public}s", eeprom_loaded ? "LOADED" : "NOT loaded");
        if (!eeprom_loaded) {
            /* A provenance signal independent of the signature: if the chip did not
             * finish loading its EEPROM, nothing read out of it can be trusted,
             * however plausible it looks. */
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

        /* Pattern checks: reject all-zeros, all-ones, and multicast. */
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

        /* All provenance checks have passed: copy the validated MAC to ivars so
         * getHardwareAddress() can serve it to the network stack. */
        for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
            ivars->macAddress[i] = mac[i];
        }
        ivars->macValid = true;

        goto mac_publish;
    }

mac_rejected:
    /* A provenance check failed, and this is the ONE place the driver decides not to
     * bring the interface up. Each check logs its own detail; this line states the
     * decision, without which the log explains what was wrong with the EEPROM but never
     * says the interface is therefore not appearing. (%{public}s, not %s: os_log
     * redacts a plain %s to <private>.)
     *
     * There is deliberately no fallback. Inventing a MAC defeats the provenance gate --
     * a stable, plausible-looking wrong value is precisely what re-read-and-compare
     * cannot catch. */
    Log("refusing to bring up the interface: %{public}s",
        macRejectReason ? macRejectReason : "MAC provenance check failed");
    ret = kIOReturnNotFound;
    goto fail;

mac_publish:
    /* ivars->macAddress can only hold a MAC that passed every provenance check by this
     * point; eeprom_loaded reflects the real auto-load status the hardware reported. */
    {
        OSData *macData = OSData::withBytes(ivars->macAddress, SMSC95XX_MAC_LEN);
        publishProperty(this, "SMSC95xxMACAddress", macData);
        OSSafeReleaseNULL(macData);
        /* OSBoolean singletons are never released, so no OSSafeReleaseNULL here. */
        publishProperty(this, "SMSC95xxEEPROMLoaded",
                        eeprom_loaded ? kOSBooleanTrue : kOSBooleanFalse);
    }

    /* Initialise the chip. The placement is load-bearing:
     *  - AFTER the MAC provenance gate above, because the sequence programs the station
     *    address into ADDRL/ADDRH and so needs a MAC it is allowed to use; a rejection
     *    has already jumped to fail before this point.
     *  - BEFORE the pool, the queues and the datapath: a chip that did not initialise
     *    has no business having any of them built, and the lite reset at the head of
     *    the sequence would reset the hardware under an in-flight transfer if the pipes
     *    were already armed.
     * A failure fails Start(): an interface that looks healthy over a chip that is not
     * receiving is worse than no interface at all. */
    {
        SMSC95xxInitIO ioCtx = { this, 0, 0 };
        smsc95xx_io    io    = { &ioCtx, initRegRead, initRegWrite };

        Log("init: starting chip initialisation for MAC %02x:%02x:%02x:%02x:%02x:%02x "
            "(promiscuous=false, %u writes expected)",
            ivars->macAddress[0], ivars->macAddress[1], ivars->macAddress[2],
            ivars->macAddress[3], ivars->macAddress[4], ivars->macAddress[5],
            (unsigned int)SMSC95XX_INIT_WRITE_COUNT);

        /* promiscuous=false: this driver programs no multicast filter, but PRMS is not
         * the way to paper over that -- it would hand the stack every frame on the
         * segment. That belongs in setPromiscuousModeEnable when it is implemented. */
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

    /* The driver's OWN Default queue, not a dedicated one. TxDataAvailable, RxComplete
     * and TxComplete carry no QUEUENAME, so DriverKit delivers them on the queue it
     * created for this IOService in init() (kIOServiceDefaultQueueName). Handing that
     * same queue to the four packet queues below is what makes "everything serialises,
     * so no locking" true by construction -- a dedicated queue would split the datapath
     * across two queues sharing one packet pool, which is a race, not a design.
     * CopyDispatchQueue returns a retained queue, so ReleaseResources' release is correct. */
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
        options.packetCount         = SMSC95XX_POOL_PACKET_COUNT;
        options.bufferCount         = SMSC95XX_POOL_PACKET_COUNT;
        options.bufferSize          = SMSC95XX_POOL_BUFFER_SIZE;
        options.maxBuffersPerPacket = 1;

        /* PoolFlagMapToDext maps the pool into this process's address space; without it
         * getDataVirtualAddress() returns pool-relative offsets, not usable addresses,
         * and the datapath memcpy faults on every frame. Direction flags are relative to
         * the DEVICE, so a pool serving both directions needs both: the device reads
         * frames we transmit and writes frames we receive. */
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

    /* All four queues built the same way, with our own dispatch queue: Create gives each
     * queue its shared-memory data queue, and the TX submission queue's data queue is the
     * doorbell setupDatapath() attaches a handler to. The withPool overload takes no
     * IODispatchQueue, and no shipping Apple network dext uses it.
     *
     * The queue IDs are distinct per role, matching the IOReport legend a working Apple
     * driver publishes: submission queues share id 0 across the two directions while the
     * completion queues take 4 and 5. */
    ret = IOUserNetworkTxSubmissionQueue::Create(ivars->pool, this,
                                                SMSC95XX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_TX_SUBMIT,
                                                ivars->dispatchQueue, &ivars->txSubmit);
    if (ret != kIOReturnSuccess) { Log("TX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkTxCompletionQueue::Create(ivars->pool, this,
                                                SMSC95XX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_TX_COMPLETE,
                                                ivars->dispatchQueue, &ivars->txComplete);
    if (ret != kIOReturnSuccess) { Log("TX completion queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxSubmissionQueue::Create(ivars->pool, this,
                                                SMSC95XX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_RX_SUBMIT,
                                                ivars->dispatchQueue, &ivars->rxSubmit);
    if (ret != kIOReturnSuccess) { Log("RX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxCompletionQueue::Create(ivars->pool, this,
                                                SMSC95XX_QUEUE_CAPACITY,
                                                SMSC95XX_QID_RX_COMPLETE,
                                                ivars->dispatchQueue, &ivars->rxComplete);
    if (ret != kIOReturnSuccess) { Log("RX completion queue failed: 0x%x", ret); goto fail; }

    Log("pool and four queues created (queue ids tx-sub %u, tx-cmp %u, rx-sub %u, rx-cmp %u)",
        SMSC95XX_QID_TX_SUBMIT, SMSC95XX_QID_TX_COMPLETE,
        SMSC95XX_QID_RX_SUBMIT, SMSC95XX_QID_RX_COMPLETE);

    /* Bulk pipes, transfer buffers and the two completion actions. Acquired after the
     * pool and the queues so that ReleaseResources' order -- datapath, queues, pool -- is the
     * exact reverse of setup. No RX read is armed here; that happens in
     * setInterfaceEnable, once the stack is ready to receive. */
    ret = setupDatapath();
    if (ret != kIOReturnSuccess) {
        Log("datapath setup failed: 0x%x", ret);
        goto fail;
    }

    {
        /* The NDK_24 overload, which takes the MAC, NOT the NDK_22 one that does not:
         * the no-MAC overload leaves the family's interface descriptor with an all-zero
         * hardware address, and Skywalk reads the descriptor as well as the getter.
         * One pool serves both directions, which the header permits; submission queues
         * first, then completion. */
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

        /* The queues are NOT enabled here; they are enabled from setInterfaceEnable(true)
         * once the interface itself is enabled. Enabling them at registration time has
         * txSubmit and rxComplete refuse with kIOReturnNotReady, so two of four queues
         * stay disabled and the stack never enqueues a transmit packet. */
    }

    /* Report active while the dongle is attached. A T1S multidrop segment without
     * PLCA carries no continuous link-integrity signal, so there is nothing truthful
     * to poll: BMSR bit 2 reads set even with the cable unplugged and must never be
     * presented as link truth. 10BASE-T is the closest available media word --
     * there is no 10BASE-T1S constant -- and the link is genuinely half duplex. */
    /* NEITHER of the two calls below fails the attach, and that is deliberate: both happen
     * AFTER registerEthernetInterface, so the family may already have called
     * setInterfaceEnable(true) -- arming a receive read and enabling the packet queues -- by
     * the time either returns. Tearing down synchronously from here could then release
     * buffers under a live handler, which is the whole hazard the Stop path was rewritten to
     * avoid. Keeping every `goto fail` site strictly BEFORE registration is what makes the
     * synchronous failure teardown below provably safe.
     *
     * Both are safe to merely log. reportLinkStatus is re-issued from
     * setInterfaceEnable(true), which is the call that actually matters (a status reported
     * before the interface is enabled is discarded anyway). RegisterService only publishes
     * the service for further matching; the interface is already registered and functional
     * without it. */
    ret = reportLinkStatus(kIOUserNetworkLinkStatusActive, SMSC95XX_MEDIA_WORD);
    if (ret != kIOReturnSuccess) {
        Log("reportLinkStatus failed: 0x%x -- continuing, setInterfaceEnable re-reports it",
            ret);
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        Log("RegisterService failed: 0x%x -- continuing, the interface is already registered",
            ret);
    }
    return kIOReturnSuccess;

fail:
    /* Synchronous teardown, unlike Stop(), and it is safe here because no handler can be in
     * flight on this path. Every goto fail site is before registerEthernetInterface, so the
     * family has never been able to call setInterfaceEnable(true): no bulk IN read is posted
     * and no packet queue is enabled. The backoff timer, although enabled, has never been
     * given a WakeAtTime deadline, so it cannot fire either. There is nothing to cancel and
     * wait for. Do not add a goto fail after registration without revisiting this. */
    quiesceDatapath();
    ::ReleaseResources(this, provider);
    Stop(provider, SUPERDISPATCH);
    return ret;
}

/* Stop is ASYNCHRONOUS: it returns without calling super, and super::Stop runs later from
 * the last dispatch-source cancellation handler.
 *
 * IOService::Stop requires this: "stop all activity and put your driver in a quiescent
 * state. If your driver has any in-progress asynchronous tasks, cancel those tasks and
 * wait for DriverKit to call the associated cancellation handler before calling the super
 * version of this method."
 *
 * Stop cannot block and wait instead. Stop is delivered ON the driver's Default queue
 * (measured: quiesceDatapath's OnQueue probe reports true here), that queue is serial, and
 * every handler below is delivered on it too -- so a cancellation handler cannot run until
 * Stop returns, and waiting for one inside Stop would deadlock. Deferring super::Stop into
 * the handler is the join.
 *
 * WHAT THE CANCELS DO AND DO NOT ACHIEVE, measured rather than assumed:
 *  - The backoff timer and the TX doorbell ARE genuinely joined. IODispatchSource::Cancel's
 *    handler fires after that source's in-flight callbacks complete, which is what makes it
 *    safe to release the sources and everything the datapath callbacks dereference. This is
 *    the defect the rewrite existed to fix, and it is fixed.
 *  - The two pipe completions are NOT joined, by anything available here. Neither the
 *    synchronous Abort nor OSAction::Cancel prevents an aborted RxComplete from arriving
 *    after the buffers have been released -- verified on builds both with and without those
 *    action Cancels. The completion handlers' own null guards are what make that safe; see
 *    the long comment in quiesceDatapath. rxAction and txAction are still counted and
 *    cancelled below, because that is the documented way to stop a FUTURE invocation.
 *
 * SetEnable(false) only stops future deliveries and says nothing about a handler already
 * executing, so it cannot replace any of these Cancels; see
 * reference/teardown-quiescence.txt for each primitive's documented guarantee.
 *
 * `this` and `provider` are retained across the asynchronous gap and released once
 * super::Stop has returned. The atomic counter is what makes super::Stop run exactly once,
 * on whichever cancellation completes last. */
kern_return_t
IMPL(SMSC95xxDriver, Stop)
{
    Log("Stop");
    Diag("Stop(provider=0x%llx) entered", DiagPtr(provider));

    /* Stop new work and stop the hardware issuing more I/O. This does NOT join anything on
     * the dispatch queue -- the Cancels below are what do that. */
    quiesceDatapath();

    uint32_t cancelCount = 0;
    if (ivars->rxBackoffTimer != nullptr) { ++cancelCount; }   /* idle-RX backoff timer   */
    if (ivars->txDataQueue    != nullptr) { ++cancelCount; }   /* TX doorbell source      */
    if (ivars->rxAction       != nullptr) { ++cancelCount; }   /* bulk IN  completion     */
    if (ivars->txAction       != nullptr) { ++cancelCount; }   /* bulk OUT completion     */
    /* The countdown lives in ivars, not on this stack frame: the handlers run after Stop
     * has returned, and the retain on `this` below keeps ivars alive until the last one. */
    ivars->stopCancelsPending = cancelCount;

    if (cancelCount == 0) {
        /* Nothing asynchronous to cancel, so the whole teardown can finish in this frame. */
        Diag("Stop: nothing to cancel -- releasing and calling super inline");
        ::ReleaseResources(this, provider);
        return Stop(provider, SUPERDISPATCH);
    }

    this->retain();
    provider->retain();

    /* One block, handed to every Cancel. Cancel copies it to the heap, which is what makes
     * passing the same local block to all four calls correct. */
    void (^finalize)(void) = ^{
        /* Release-acquire so the countdown joins the handlers' effects even if the two
         * cancellation handlers are ever delivered on different queues. */
        uint32_t prior = __c11_atomic_fetch_sub(&ivars->stopCancelsPending, 1U,
                                                __ATOMIC_ACQ_REL);
        if (prior == 0) {
            Log("Stop: a cancellation handler ran with none outstanding -- the cancel "
                "count is wrong if this line ever appears");
        }
        if (prior != 1) {
            return;
        }
        /* Last cancellation: every handler has finished, so the objects the datapath
         * callbacks dereference can finally be released, and super can run.
         *
         * Copied to stack locals first: ReleaseResources drops the two dispatch sources,
         * and this block is running out of storage owned by one of them, so nothing after
         * that point may reach through `this` or read a captured variable again. */
        SMSC95xxDriver *self = this;
        IOService      *prov = provider;
        Diag("Stop: final cancellation handler -- releasing resources and calling super");
        ::ReleaseResources(self, prov);
        /* Logged BEFORE super, not after: super::Stop can tear this service down and the
         * process with it, so a line placed after it may never be emitted -- which is
         * precisely why the first hardware run could not confirm this point was reached. */
        Log("Stop: joined all cancellations, calling super::Stop now");
        self->Stop(prov, SUPERDISPATCH);
        prov->release();
        self->release();
    };

    /* A failed Cancel means its handler will NEVER run, so the countdown would never reach
     * zero, super::Stop would never be called, and both retains would leak -- silently.
     * Invoking finalize() directly for a failed Cancel keeps the countdown honest, so
     * super::Stop still runs exactly once whatever happens here. */
    if (ivars->rxBackoffTimer != nullptr) {
        kern_return_t cr = ivars->rxBackoffTimer->Cancel(finalize);
        if (cr != kIOReturnSuccess) {
            Log("Stop: Cancel(RX backoff timer) FAILED: 0x%x -- its handler will not run, "
                "so that cancellation is completed here instead", cr);
            finalize();
        }
    }
    if (ivars->txDataQueue != nullptr) {
        kern_return_t cr = ivars->txDataQueue->Cancel(finalize);
        if (cr != kIOReturnSuccess) {
            Log("Stop: Cancel(TX doorbell) FAILED: 0x%x -- its handler will not run, so "
                "that cancellation is completed here instead", cr);
            finalize();
        }
    }
    /* The two pipe completions. These are what make releasing rxBuffer/txBuffer safe: the
     * synchronous Abort above does not wait for a completion that is queued behind this
     * Stop on the serial queue, so without these the buffers could be released while an
     * aborted completion was still pending delivery. */
    if (ivars->rxAction != nullptr) {
        kern_return_t cr = ivars->rxAction->Cancel(finalize);
        if (cr != kIOReturnSuccess) {
            Log("Stop: Cancel(bulk IN completion) FAILED: 0x%x -- its handler will not run, "
                "so that cancellation is completed here instead", cr);
            finalize();
        }
    }
    if (ivars->txAction != nullptr) {
        kern_return_t cr = ivars->txAction->Cancel(finalize);
        if (cr != kIOReturnSuccess) {
            Log("Stop: Cancel(bulk OUT completion) FAILED: 0x%x -- its handler will not run, "
                "so that cancellation is completed here instead", cr);
            finalize();
        }
    }

    Diag("Stop: returning without super; %u cancellation(s) issued", cancelCount);
    return kIOReturnSuccess;
}

/* The capital-S deprecated form. The live one the stack calls on
 * `ifconfig up`/`down` is the lowercase setInterfaceEnable below; this exists only to
 * satisfy the SDK's declaration. */
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
    /* Accepted but not yet applied: MAC_CR.PRMS stays clear until a read-modify-write
     * of MAC_CR lands here. Logged so the gap stays visible rather than assumed. */
    Log("setPromiscuousModeEnable(%{public}s) -- accepted, but MAC_CR.PRMS is not "
        "toggled yet (Start() leaves it clear)", enable ? "true" : "false");
    return kIOReturnSuccess;
}

/* The live MTU pair. Exactly one MTU is supported: the pool buffers are 2048 bytes and
 * there is no scatter-gather, so anything larger cannot be transmitted. */
IOReturn
SMSC95xxDriver::setMaxTransferUnit(uint32_t mtu)
{
    /* Refuse before chaining, so the family never records an MTU this driver
     * then rejects. */
    if (mtu != 1500) {
        Log("setMaxTransferUnit(%u) refused -- only 1500 is supported", mtu);
        return kIOReturnUnsupported;
    }
    return super::setMaxTransferUnit(mtu);
}

uint32_t
SMSC95xxDriver::getMaxTransferUnit()
{
#if SMSC95XX_TRACE
    uint32_t superMtu = super::getMaxTransferUnit();
    Diag("getMaxTransferUnit(): super -> %u; ours -> 1500", superMtu);
#endif
    return 1500;
}

IOReturn
SMSC95xxDriver::setInterfaceEnable(bool enable)
{
    Log("setInterfaceEnable(%{public}s)", enable ? "true" : "false");

    /* Chain to the base class FIRST, and do not swallow its result:
     * IOUserNetworkEthernet::setInterfaceEnable is what puts the packet queues into a
     * state where the stack will enqueue to them. Without it the interface looks healthy
     * -- UP, RUNNING, correct media -- while the stack never hands it a packet.
     *
     * super:: rather than SUPERDISPATCH because setInterfaceEnable is LOCALONLY
     * (IOUserNetworkEthernet.iig:205): an ordinary in-process C++ virtual call. */
    IOReturn ret = super::setInterfaceEnable(enable);
    Diag("setInterfaceEnable(%{public}s): super -> 0x%x",
         enable ? "true" : "false", ret);
    if (ret != kIOReturnSuccess) {
        Log("super::setInterfaceEnable(%{public}s) failed: 0x%x",
            enable ? "true" : "false", ret);
        return ret;
    }

    if (enable) {
        /* Enable the four packet queues here, after super has enabled the interface,
         * not in Start(): before the interface is enabled, txSubmit and rxComplete
         * refuse setEnable(true) with kIOReturnNotReady and the stack never enqueues
         * a transmit packet. setEnable, not SetEnable: the capital form is the
         * deprecated one. */
        static const char *const queueNames[4] = {
            "tx submission", "rx submission", "tx completion", "rx completion"
        };
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->txSubmit, ivars->rxSubmit, ivars->txComplete, ivars->rxComplete
        };
        uint32_t enabledCount = 0;

        for (uint32_t i = 0; i < 4; i++) {
            if (queues[i] != nullptr) {
                ret = queues[i]->setEnable(true);
                if (ret != kIOReturnSuccess) {
                    Log("setEnable(true) failed on the %{public}s queue: 0x%x",
                        queueNames[i], ret);
                    goto enable_failed;
                }
            }
            enabledCount = i + 1;
        }
        Log("all four packet queues enabled");

        /* Report the link when the stack brings the interface up: a status reported
         * during Start() is discarded because the interface is not yet enabled. */
        ret = reportLinkStatus(kIOUserNetworkLinkStatusActive, SMSC95XX_MEDIA_WORD);
        if (ret != kIOReturnSuccess) {
            Log("reportLinkStatus in setInterfaceEnable failed: 0x%x", ret);
            goto enable_failed;
        }

        /* Arm the first bulk IN read only now: the RX completion queue refuses
         * deliveries until the interface is enabled, so an earlier completion would
         * have every frame allocated, copied, refused and thrown away. A failure
         * fails the enable rather than leaving an interface that cannot receive.
         *
         * rxRunning is set BEFORE arming: everything serialises on one dispatch
         * queue, so the completion cannot run until this returns, and ordering the
         * state change first keeps "a read is only outstanding while rxRunning"
         * true throughout. The idle-backoff state starts from a clean slate so empty
         * completions from a previous session cannot push a freshly enabled
         * interface straight into backoff. */
        ivars->rxRunning       = true;
        ivars->rxIdleRun       = 0;
        ivars->rxBackoffActive = false;
        ret = armRxRead();
        Diag("setInterfaceEnable(true): armRxRead -> 0x%x", ret);
        if (ret != kIOReturnSuccess) {
            Log("setInterfaceEnable: failed to arm the first RX read: 0x%x", ret);
            ivars->rxRunning = false;
            goto enable_failed;
        }
        Log("setInterfaceEnable: RX armed");
        return kIOReturnSuccess;

enable_failed:
        /* Unwind so the family and the driver agree the interface is down. Without
         * this, a mid-enable failure leaves the interface enabled at the family
         * level with some queues live and, on the arm failure path, a link already
         * reported active over a receive path that is dead. Unwind errors are logged
         * and otherwise ignored: `ret` must keep the original failure. */
        for (uint32_t i = 0; i < enabledCount; i++) {
            if (queues[i] != nullptr) {
                IOReturn er = queues[i]->setEnable(false);
                if (er != kIOReturnSuccess) {
                    Log("enable unwind: setEnable(false) failed on the %{public}s "
                        "queue: 0x%x", queueNames[i], er);
                }
            }
        }
        reportLinkStatus(kIOUserNetworkLinkStatusInactive, SMSC95XX_MEDIA_WORD);
        IOReturn dr = super::setInterfaceEnable(false);
        if (dr != kIOReturnSuccess) {
            Log("enable unwind: super::setInterfaceEnable(false) failed: 0x%x", dr);
        }
        return ret;
    } else {
        /* Let the receive loop wind down. The outstanding transfer is deliberately NOT
         * aborted -- that belongs to teardown -- so one more completion can still arrive;
         * RxComplete sees rxRunning false, discards it and does not re-arm. */
        ivars->rxRunning = false;
        Log("setInterfaceEnable(false): RX will stop after the outstanding read completes");
        /* Log totals now as well as at teardown, so a disable/re-enable cycle without a
         * dext restart is summarised per run rather than only once, added together. */
        logDatapathCounters("interface disable");
    }
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::SetMulticastAddresses_Impl(const IOUserNetworkMACAddress *addresses __unused,
                                           uint32_t count __unused)
{
    /* No multicast filtering. Refusing is honest: Start() writes MAC_CR with PRMS clear
     * and HASHH/HASHL zeroed, so the hardware filters to the station address and
     * broadcast rather than passing everything. Hash filtering is follow-on work. */
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SetAllMulticastModeEnable_Impl(bool enable __unused)
{
    /* No multicast filtering; see SetMulticastAddresses_Impl above. */
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

/* IOUserNetworkEthernet.iig:309 -- getHardwareAddress(ether_addr_t *) LOCALONLY NDK_21.
 * ether_addr_t (octet[6]) is not the deprecated IOUserNetworkMACAddress used by the
 * older RegisterEthernetInterface overload. */
kern_return_t
SMSC95xxDriver::getHardwareAddress(ether_addr_t *addr)
{
#if SMSC95XX_TRACE
    {
        /* The family default is read into a LOCAL buffer and only logged; the MAC
         * provenance gate below is the only thing that decides what the stack sees. */
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
#endif

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
 *
 * Three entries: none, autoselect, then the one real medium. The first two are what
 * System Settings builds its Speed/Duplex dropdowns from, and the family's own initial
 * media is kIOUserNetworkMediaEthernetAuto -- so a list without autoselect leaves the
 * interface starting in a medium absent from its own supported list. Working adapters
 * list none and autoselect first; this matches that shape.
 *
 * mediaCount is an OUTPUT, not a capacity: the stack calls with *mediaCount == 0 and a
 * non-null array, and treating it as a capacity produces SIOCGIFXMEDIA I/O errors. The
 * API offers no way to learn the array's real size; three entries is well inside the
 * smallest list any shipping adapter reports. */
IOReturn
SMSC95xxDriver::getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount)
{
#if SMSC95XX_TRACE
    {
        /* Chain into a LOCAL array, never the caller's, so the entries we advertise
         * reach the family unchanged. 256 words is the most the family transport
         * (_ReportAvailableMediaTypes) can carry. */
        MediaWord superArray[256] = {};
        uint32_t  superCount = 0;
        IOReturn  superRet   = super::getSupportedMediaArray(superArray, &superCount);
        MediaWord superFirst = (superCount > 0 && superCount <= 256) ? superArray[0] : 0u;
        Diag("getSupportedMediaArray: super -> 0x%x, count %u, first 0x%08x; "
             "ours -> count 3, {none, auto, 0x%08x}",
             superRet, superCount, superFirst, (uint32_t)SMSC95XX_MEDIA_WORD);
    }
#endif

    if (mediaCount == nullptr) {
        return kIOReturnBadArgument;
    }
    if (mediaArray != nullptr) {
        mediaArray[0] = kIOUserNetworkMediaEthernetNone;
        mediaArray[1] = kIOUserNetworkMediaEthernetAuto;
        mediaArray[2] = SMSC95XX_MEDIA_WORD;
    }
    *mediaCount = 3;
    return kIOReturnSuccess;
}

/* Return 0 to prevent the stack from using offloads we do not implement.
 * The deprecated capital-G GetHardwareAssists_Impl is not called by the stack;
 * this is the live method per IOUserNetworkEthernet.iig:228 (LOCALONLY NDK_21). */
uint32_t
SMSC95xxDriver::getHardwareAssists()
{
#if SMSC95XX_TRACE
    uint32_t superAssists = super::getHardwareAssists();
    Diag("getHardwareAssists(): super -> 0x%08x; ours -> 0x00000000 (no offloads)",
         superAssists);
#endif
    return 0;
}

/* Advertise no TSO. Per IOUserNetworkEthernet.iig:375 (LOCALONLY NDK_22). */
IOReturn
SMSC95xxDriver::getTSOOptions(IOUserNetworkTSOOptions *options)
{
#if SMSC95XX_TRACE
    IOUserNetworkTSOOptions superOptions = {};
    IOReturn superRet = super::getTSOOptions(&superOptions);
    Diag("getTSOOptions: super -> 0x%x mss4 %u mss6 %u; ours -> mss4 0 mss6 0",
         superRet, superOptions.mss4, superOptions.mss6);
#endif
    if (options == nullptr) {
        return kIOReturnBadArgument;
    }
    /* Zero the WHOLE struct: it is a union of {tso_mtu4, tso_mtu6} plus a 56-byte
     * reserved tail, so assigning the two named fields would leave 56 caller-provided
     * bytes untouched while claiming to be zero-initialised. */
    *options = IOUserNetworkTSOOptions{};
    return kIOReturnSuccess;
}

/* Refuse every hardware assist; this hardware offers none. The requested bits are
 * logged because a stack that expects an assist to stick is worth being able to see.
 * Per IOUserNetworkEthernet.iig:369 (LOCALONLY NDK_22). */
IOReturn
SMSC95xxDriver::setHardwareAssists(uint32_t hardwareAssists, uint32_t hardwareAssistsMask)
{
    /* Refused without chaining, so the family cannot record assist state this
     * driver then disowns. */
    Log("setHardwareAssists: requested=0x%08x, mask=0x%08x -- refused (no offloads)",
        hardwareAssists, hardwareAssistsMask);
    return kIOReturnUnsupported;
}

/* IOUserNetworkEthernet.iig:360 -- getTxDataOffset() LOCALONLY. The data offset the
 * stack reserves ahead of every TX packet, i.e. room for the 8-byte TX command header.
 * The family default is 0; this chip needs 8.
 *
 * getDataVirtualAddress() returns the buffer BASE and does NOT include this offset --
 * the frame begins at base + getDataOff(). Both datapaths add getDataOff() explicitly,
 * so this arithmetic is correct for any value returned here. */
uint16_t
SMSC95xxDriver::getTxDataOffset()
{
    return (uint16_t)SMSC95XX_TX_HEADER_LEN;
}
