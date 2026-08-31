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
#include <USBDriverKit/IOUSBHostDevice.h>
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
#include "smsc95xx_regs.h"
#include "smsc95xx_proto.h"

/* One definition, used by both reportLinkStatus call sites and getSupportedMediaArray,
 * so the advertised media and the reported active media cannot drift apart.
 * 10BASE-T is the closest available word -- there is no 10BASE-T1S constant -- and the
 * segment is genuinely half duplex. */
#define SMSC95XX_MEDIA_WORD \
    (kIOUserNetworkMediaEthernet10BaseT | kIOUserNetworkMediaOptionHalfDuplex)

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: " fmt, ##__VA_ARGS__)

/* Poll attempts for MII and EEPROM operations */
#define POLL_ATTEMPTS 100

/* `device` is the matched provider; `interface` is copied from it in Start once the
 * device is configured. Both are populated during Start().
 * `ctrlBuffer` is a 4-byte IOBufferMemoryDescriptor used for all vendor control
 * transfers (reads and writes). `ctrlBytes` is the mapped address of ctrlBuffer. */
struct SMSC95xxDriver_IVars {
    IOUSBHostDevice         *device;
    IOUSBHostInterface      *interface;
    IOBufferMemoryDescriptor *ctrlBuffer;
    uint8_t                 *ctrlBytes;
    /* CopyInterface hands back a retained object, so `interface` can be non-null
     * while unopened. Closing in that state would unbalance IOKit's opener
     * accounting, so track the two facts separately. */
    bool                     interfaceOpened;
    /* The stack fetches the MAC through getHardwareAddress() rather than as a
     * registration argument, so the validated bytes have to outlive Start(). */
    uint8_t                  macAddress[SMSC95XX_MAC_LEN];
    bool                     macValid;
    IODispatchQueue                *netDispatchQueue;
    IOUserNetworkPacketBufferPool  *pool;
    IOUserNetworkTxSubmissionQueue *txSubmit;
    IOUserNetworkRxSubmissionQueue *rxSubmit;
    IOUserNetworkTxCompletionQueue *txComplete;
    IOUserNetworkRxCompletionQueue *rxComplete;
};

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

/* The LAN9500A has exactly one interface, so take the first the iterator yields. */
static kern_return_t
CopyFirstInterface(IOUSBHostDevice *device, IOUSBHostInterface **out)
{
    uintptr_t iter = 0;
    kern_return_t ret;

    *out = nullptr;
    ret = device->CreateInterfaceIterator(&iter);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = device->CopyInterface(iter, out);
    device->DestroyInterfaceIterator(iter);
    return ret;
}

/* Teardown path for partial Start() failures. Must be safe to call after any
 * subset of ivars has been populated. Called from both the fail: label in Start()
 * and from Stop(). Release the four queues before the pool: they hold references
 * to it, and tearing the pool down first is the kind of ordering bug that only
 * shows up on an error path. Release the dispatch queue last. */
static kern_return_t
TearDown(SMSC95xxDriver *driver, IOService *provider __unused)
{
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
        OSSafeReleaseNULL(driver->ivars->interface);
    }
    driver->ivars->ctrlBytes = nullptr;
    OSSafeReleaseNULL(driver->ivars->ctrlBuffer);
    if (driver->ivars->device != nullptr) {
        driver->ivars->device->Close(driver, 0);
        /* Borrowed from the provider: close it, never release it, and null it so a
         * second TearDown cannot issue a second Close. */
        driver->ivars->device = nullptr;
    }

    /* Release the dispatch queue last. */
    OSSafeReleaseNULL(driver->ivars->netDispatchQueue);

    return kIOReturnSuccess;
}

kern_return_t
IMPL(SMSC95xxDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("Start(super) failed: 0x%x", ret);
        return ret;
    }

    Log("Start: matched provider");

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Log("provider is not an IOUSBHostDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    /* Note the argument type: IOUSBHostDevice::Open takes uintptr_t for arg, while
     * IOUSBHostInterface::Open takes uint8_t *. They are not interchangeable. */
    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    /* DriverKit's IOUSBHostDevice has no GetConfiguration() -- that is IOUSBLib's API,
     * not this one. Probing for an interface answers the same question directly: child
     * interfaces exist only once a configuration is selected. macOS leaves this device
     * unconfigured (measured on a clean attach in the 0x9E00 state), and registers at
     * offsets >= 0x100 stall until it is configured. Re-issuing SET_CONFIGURATION
     * terminates existing child interfaces, so only do it when none were found. */
    ret = CopyFirstInterface(ivars->device, &ivars->interface);
    if (ret != kIOReturnSuccess || ivars->interface == nullptr) {
        ret = ivars->device->SetConfiguration(1, false);
        if (ret != kIOReturnSuccess) {
            Log("SetConfiguration(1) failed: 0x%x", ret);
            goto fail;
        }
        Log("device arrived unconfigured; selected configuration 1");

        ret = CopyFirstInterface(ivars->device, &ivars->interface);
        if (ret != kIOReturnSuccess || ivars->interface == nullptr) {
            Log("no interface after SetConfiguration: 0x%x", ret);
            ret = (ret == kIOReturnSuccess) ? kIOReturnNoDevice : ret;
            goto fail;
        }
    } else {
        Log("device already configured");
    }

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
        bool eeprom_loaded = false;

        /* Check if EEPROM auto-load succeeded */
        {
            uint32_t e2p_cmd = 0;
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
                goto fail;
            }
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
            goto fail;
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
            goto fail;
        }

        Log("MAC %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        /* All provenance checks have passed: copy the validated MAC to ivars so it
         * can be served via getHardwareAddress() to the network stack. */
        for (int i = 0; i < SMSC95XX_MAC_LEN; i++) {
            ivars->macAddress[i] = mac[i];
        }
        ivars->macValid = true;

        /* Publish MAC and EEPROM status to ioreg */
        OSDictionary *props = OSDictionary::withCapacity(2);
        if (props != nullptr) {
            /* Publish MAC address as OSData */
            OSData *macData = OSData::withBytes(mac, SMSC95XX_MAC_LEN);
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

    /* We pass this queue to the four Create() calls ourselves, which is why no
     * TxDispatchQueue/RxDispatchQueue entry is needed in Info.plist. */
    ret = IODispatchQueue::Create("SMSC95xxNetQueue", 0, 0, &ivars->netDispatchQueue);
    if (ret != kIOReturnSuccess) {
        Log("dispatch queue creation failed: 0x%x", ret);
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

        ret = IOUserNetworkPacketBufferPool::CreateWithOptions(this, "SMSC95xxPool",
                                                              &options, &ivars->pool);
        if (ret != kIOReturnSuccess) {
            Log("packet buffer pool creation failed: 0x%x", ret);
            goto fail;
        }
    }

    ret = IOUserNetworkTxSubmissionQueue::Create(ivars->pool, this, 64, 0,
                                                ivars->netDispatchQueue, &ivars->txSubmit);
    if (ret != kIOReturnSuccess) { Log("TX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkTxCompletionQueue::Create(ivars->pool, this, 64, 0,
                                                ivars->netDispatchQueue, &ivars->txComplete);
    if (ret != kIOReturnSuccess) { Log("TX completion queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxSubmissionQueue::Create(ivars->pool, this, 64, 0,
                                                ivars->netDispatchQueue, &ivars->rxSubmit);
    if (ret != kIOReturnSuccess) { Log("RX submission queue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxCompletionQueue::Create(ivars->pool, this, 64, 0,
                                                ivars->netDispatchQueue, &ivars->rxComplete);
    if (ret != kIOReturnSuccess) { Log("RX completion queue failed: 0x%x", ret); goto fail; }

    Log("pool and four queues created");

    {
        /* NDK_22 lowercase overload: no MAC argument -- the stack calls
         * getHardwareAddress() instead. One pool serves both directions, which the
         * header explicitly permits. Submission queues first, then completion. */
        IOUserNetworkPacketQueue *queues[4] = {
            ivars->txSubmit, ivars->rxSubmit, ivars->txComplete, ivars->rxComplete
        };

        ret = registerEthernetInterface(queues, 4, ivars->pool, ivars->pool);
        if (ret != kIOReturnSuccess) {
            Log("registerEthernetInterface failed: 0x%x", ret);
            goto fail;
        }
        Log("ethernet interface registered");
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
    /* Accepted but NOT yet acted on: this dext programs no MAC filter, because it never
     * writes MAC_CR. Returning success is therefore a promise M5 has to keep by setting
     * MAC_CR.PRMS. Logged so the gap is visible rather than assumed. */
    Log("setPromiscuousModeEnable(%{public}s) -- accepted, but no MAC filter is programmed yet",
        enable ? "true" : "false");
    return kIOReturnSuccess;
}

/* The live MTU pair. v1 carries exactly one MTU: the pool buffers are 2048 bytes and
 * there is no scatter-gather, so anything larger cannot be transmitted. */
IOReturn
SMSC95xxDriver::setMaxTransferUnit(uint32_t mtu)
{
    if (mtu != 1500) {
        Log("setMaxTransferUnit(%u) refused -- only 1500 is supported", mtu);
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

uint32_t
SMSC95xxDriver::getMaxTransferUnit()
{
    return 1500;
}

IOReturn
SMSC95xxDriver::setInterfaceEnable(bool enable)
{
    Log("setInterfaceEnable(%{public}s)", enable ? "true" : "false");
    if (enable) {
        /* Report the link active when the stack brings the interface up, not just at
         * registration time. A link status reported during Start() is discarded because
         * the interface is not yet enabled; this call happens when the user (or the
         * system) actually enables it via ifconfig or similar. */
        IOReturn ret = reportLinkStatus(kIOUserNetworkLinkStatusActive,
                                       kIOUserNetworkMediaEthernet10BaseT |
                                       kIOUserNetworkMediaOptionHalfDuplex);
        if (ret != kIOReturnSuccess) {
            Log("reportLinkStatus in setInterfaceEnable failed: 0x%x", ret);
            return ret;
        }
    }
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::SetMulticastAddresses_Impl(const IOUserNetworkMACAddress *addresses __unused,
                                           uint32_t count __unused)
{
    /* v1 has no multicast filtering. NOTE: this dext programs no MAC filter at all --
     * it never writes MAC_CR -- so refusing here is not currently backed by promiscuous
     * hardware. M5 must set MAC_CR (RXEN, and PRMS or real hash filtering) before this
     * return value can be called safe. */
    return kIOReturnUnsupported;
}

kern_return_t
SMSC95xxDriver::SetAllMulticastModeEnable_Impl(bool enable __unused)
{
    /* v1 has no multicast filtering. NOTE: this dext programs no MAC filter at all --
     * it never writes MAC_CR -- so refusing here is not currently backed by promiscuous
     * hardware. M5 must set MAC_CR (RXEN, and PRMS or real hash filtering) before this
     * return value can be called safe. */
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
IOReturn
SMSC95xxDriver::getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount)
{
    if (mediaCount == nullptr) {
        return kIOReturnBadArgument;
    }

    /* mediaCount is an output field, not a capacity field. The stack always provides
     * a non-null array and we write exactly one entry, setting the count to 1. */
    if (mediaArray != nullptr) {
        mediaArray[0] = SMSC95XX_MEDIA_WORD;
        *mediaCount   = 1;
        return kIOReturnSuccess;
    }

    /* If mediaArray is null (should not happen), report the count anyway for safety. */
    *mediaCount = 1;
    return kIOReturnSuccess;
}

/* Return 0 to prevent the stack from using offloads we do not implement.
 * The deprecated capital-G GetHardwareAssists_Impl is not called by the stack;
 * this is the live method per IOUserNetworkEthernet.iig:228 (LOCALONLY NDK_21). */
uint32_t
SMSC95xxDriver::getHardwareAssists()
{
    Log("getHardwareAssists: returning 0 (no offloads)");
    return 0;
}

/* Experiment: test whether explicit TSO options suppress the offload flags.
 * Per IOUserNetworkEthernet.iig:375 (LOCALONLY NDK_22). Returns a zero-initialised
 * struct, which should advertise that we support no TSO. */
IOReturn
SMSC95xxDriver::getTSOOptions(IOUserNetworkTSOOptions *options)
{
    if (options == nullptr) {
        return kIOReturnBadArgument;
    }
    /* Zero the WHOLE struct: it is a union of {tso_mtu4, tso_mtu6} plus a 56-byte
     * reserved tail, so assigning the two named fields would leave 56 caller-provided
     * bytes untouched while claiming to be zero-initialised. */
    *options = IOUserNetworkTSOOptions{};
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
    return kIOReturnUnsupported;
}
