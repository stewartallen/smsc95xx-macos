/* SPDX-License-Identifier: GPL-2.0
 *
 * The data path: bulk pipes, transfer buffers, and the TX/RX loops. Kept separate from
 * SMSC95xxDriver.cpp, which owns the control path (registers, MII, EEPROM, chip init).
 */

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSAction.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
/* kIOUSBAbortSynchronous lives here, not in IOUSBHostFamilyDefinitions.h, and only
 * USBDriverKit.h includes it for you -- so include it explicitly. Getting this wrong and
 * falling back to a bare 0 would silently select kIOUSBAbortAsynchronous. */
#include <USBDriverKit/USBDriverKitDefs.h>
/* kUSBHostReturnPipeStalled (0xe0005000) lives here. Note the spelling: there is no
 * kIOUSBPipeStalled anywhere in the DriverKit SDK -- that is the kernel-framework name --
 * and this header is self-contained, so including it costs nothing. */
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>
#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
/* The idle-RX backoff timer. */
#include <DriverKit/IOTimerDispatchSource.h>
/* clock_gettime_nsec_np(CLOCK_UPTIME_RAW), which is the timebase
 * IOTimerDispatchSource::WakeAtTime documents as kIOTimerClockUptimeRaw and recommends in
 * preference to raw mach ticks. Declared in the DriverKit SDK's <time.h> and exported from
 * libSystem, so this needs no extra link flag. */
#include <time.h>
#include <NetworkingDriverKit/IOUserNetworkPacket.h>
/* ivars->txSubmit and ivars->txComplete are only forward-declared in the ivars header, so
 * the concrete types have to be visible here to call DequeuePackets()/enqueuePackets(). */
#include <NetworkingDriverKit/IOUserNetworkTxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkTxCompletionQueue.h>
/* Same reason, for the receive half: ivars->rxSubmit is dequeued from, ivars->rxComplete is
 * enqueued to, and ivars->pool is the fallback allocator. */
#include <NetworkingDriverKit/IOUserNetworkRxSubmissionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkRxCompletionQueue.h>
#include <NetworkingDriverKit/IOUserNetworkPacketBufferPool.h>

#include "SMSC95xxDriver.h"
#include "SMSC95xxDriver_ivars.h"
#include "smsc95xx_proto.h"
#include "smsc95xx_regs.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: " fmt, ##__VA_ARGS__)

/* THE PER-FRAME TRACES. Both are compiled in only with `make TRACE=1`, and the Makefile's
 * TRACE block explains why they must stay out of a normal build: these are the lines that fire
 * once per frame, and os_log throttling under a burst has already destroyed one attach's log
 * completely. What remains in a quiet build is every error, every refusal, every guard, and
 * logDatapathCounters()'s two lines of totals -- which is what answers "did it work" after the
 * fact. These answer "what happened to this particular frame", which is a question worth
 * asking only while a bug is being chased.
 *
 * Disabled, they compile to `if (0) os_log(...)` rather than to nothing, so every format
 * string and argument stays type-checked in a default build. See the DIAG5 macro in
 * SMSC95xxDriver.cpp for the full reasoning, including why discarding the evaluation is safe.
 *
 * DIAG7 -- the receive half. Every completion, every record, every drop with its reason,
 * every enqueue and every re-arm:
 *   log stream --predicate 'eventMessage CONTAINS "DIAG7:"'
 *
 * DIAG6 -- the transmit half. Every doorbell, every dequeue, every framed transfer with its
 * length, every completion with its status and byte count, and every packet handed back to
 * the stack. That last one matters more than it looks: the failure mode of getting the
 * accounting wrong -- a packet consumed and never completed -- is silent for exactly 64
 * packets and then looks like a hang:
 *   log stream --predicate 'eventMessage CONTAINS "DIAG6:"'
 *
 * All three prefixes together, plus everything else this driver logs:
 *   log stream --predicate 'eventMessage CONTAINS "SMSC95xx"'
 */
#if SMSC95XX_TRACE
#define Diag7(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG7: " fmt, ##__VA_ARGS__)
#define Diag6(fmt, ...) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG6: " fmt, ##__VA_ARGS__)
#else
#define Diag7(fmt, ...) \
    do { if (0) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG7: " fmt, ##__VA_ARGS__); } while (0)
#define Diag6(fmt, ...) \
    do { if (0) os_log(OS_LOG_DEFAULT, "SMSC95xx: DIAG6: " fmt, ##__VA_ARGS__); } while (0)
#endif

/* Endpoint addresses from reference/mach-lan9500a-descriptors.txt; identical on the EVB.
 * Both are bulk with wMaxPacketSize 512. 0x83 is an interrupt IN endpoint carrying
 * link-status notifications, unused in v1. */
#define SMSC95XX_EP_BULK_IN    0x81
#define SMSC95XX_EP_BULK_OUT   0x02

/* One RX transfer holds several frames: the chip packs them back to back with a 4-byte
 * status word each when HW_CFG.MEF is set. 4096 comfortably holds two full-size frames
 * plus headers and padding. The host chooses the transfer length, so the device cannot
 * overrun this. */
#define SMSC95XX_RX_BUFFER_LEN 4096
/* Largest frame the transmit path will copy into the TX buffer, as a memory-safety bound.
 *
 * NOT an offload question -- that one is closed. Measured over a real run, the lengths the
 * stack hands this driver were 42 to 1372 with a maximum of 1372 and nothing at all over
 * 1514: the TSO4/TSO6/PARTIAL_CSUM in `ifconfig` is Skywalk performing those offloads in
 * software above the driver, not a request for the driver to perform them. This bound stays
 * because the memcpy below is only as safe as the length driving it, and 1518 (a VLAN-tagged
 * maximum-length frame) is the largest thing that could legitimately arrive. Anything above
 * it is corruption, and the frame is refused rather than copied. Note that
 * smsc95xx_tx_prepend applies the stricter protocol limit of SMSC95XX_FRAME_MAX (1514)
 * immediately afterwards, so 1515..1518 is refused too -- just with a different log line,
 * which is the point: "implausible length" and "the protocol says no" are different faults. */
#define SMSC95XX_TX_MAX_FRAME 1518

/* Worst case one frame plus its 8-byte TX header, and the exact bound smsc95xx_tx_prepend is
 * given so a short buffer could never be written past. SMSC95XX_TX_MAX_FRAME rather than
 * SMSC95XX_FRAME_MAX (1514) leaves the four bytes of slack a CRC would occupy; the chip
 * appends the CRC itself, so those bytes are headroom, not payload. */
#define SMSC95XX_TX_BUFFER_LEN (SMSC95XX_TX_HEADER_LEN + SMSC95XX_TX_MAX_FRAME)

/* How many transmitted packets get the full framing trace, including the data offset the
 * family handed us. The offset answers whether getTxDataOffset()'s 8 bytes of headroom were
 * actually reserved, which is a fixed property of the family and the pool -- so it is worth
 * a handful of lines at the start of each attach and nothing after that. Every transmit is
 * still logged with its length; this is the extra detail. */
#define SMSC95XX_TX_TRACE_PACKETS 8

/* Frames are handed to the stack ONE AT A TIME, not batched. This is not a simplification:
 * enqueuePackets() reports only whether the accepted count was zero, so a partially accepted
 * batch is indistinguishable from a fully accepted one and the refused packets leak. See
 * deliverPacket() for the disassembly that establishes it. A batch constant used to live here.
 *
 * How many received packets get the full acquisition trace. The question it answers -- does
 * the RX submission queue actually supply buffers -- is a fixed property of the family, so it
 * is worth a handful of lines at the start of an attach and nothing after that. */
#define SMSC95XX_RX_TRACE_PACKETS 8

/* Largest RX payload (frame without its CRC) that will be copied into a packet.
 *
 * This is a memory-safety bound, not a policy: smsc95xx_rx_next only guarantees that a
 * record fits inside the transfer buffer, so a corrupt status word can legitimately yield a
 * length of nearly SMSC95XX_RX_BUFFER_LEN -- and the pool's buffers are 2048 bytes
 * (SMSC95xxDriver.cpp), so copying that blindly would overrun the packet. 1518 is a
 * VLAN-tagged maximum-length Ethernet frame, i.e. the largest thing this interface could
 * legitimately receive; anything above it is either corruption or a configuration this
 * driver does not support, and either way must be dropped rather than copied. It is
 * deliberately larger than SMSC95XX_FRAME_MAX (1514) so that a tagged frame is not silently
 * discarded, and comfortably smaller than 2048 so the copy always fits. */
#define SMSC95XX_RX_MAX_PAYLOAD 1518

/* The pool's per-packet buffer size, as passed to CreateWithOptions in SMSC95xxDriver.cpp.
 * Duplicated here rather than shared because the two files describe different things -- there
 * it is a pool parameter, here it is the bound on a memcpy -- but they must agree.
 *
 * Both directions need it, and neither can use a frame-length bound alone: the copy starts at
 * getDataOff() bytes into the buffer, so offset plus length is what has to fit. On TX that
 * bounds a READ past the end of a packet, which is exactly as fatal as a bad write -- a fault
 * on every frame is a kernel panic by way of IOKit's respawn loop, not merely a crash. */
#define SMSC95XX_POOL_BUFFER_SIZE 2048

/* ---- the idle-RX backoff -------------------------------------------------------------
 *
 * THE DEFECT THIS FIXES: RxComplete used to re-arm unconditionally on success, and a
 * zero-length completion counts as success. A device that has nothing to give completes
 * the next read immediately, so the two form a tight loop with no I/O wait in it at all --
 * measured at ~364,000 arms and 19% of a CPU core. The immediate cause was that the chip
 * was never initialised, which is fixed separately in Start(); this guard is independent of
 * that, because "the device has nothing to say" must never cost measurable CPU whatever the
 * reason. An error completion is treated the same way: a stalled pipe that fails instantly
 * has exactly the same shape.
 *
 * SMSC95XX_RX_IDLE_RUN_LIMIT -- how many consecutive completions that delivered nothing are
 * re-armed immediately before the timer takes over. 8 is chosen to be comfortably above any
 * legitimate transient (the completion that can follow an abort, the first read after
 * enable, one lost transfer) and far below the point where spinning costs anything: eight
 * control-free iterations is microseconds of work, against the 364,000 the unguarded loop
 * produced.
 *
 * SMSC95XX_RX_BACKOFF_NS -- 100 ms, giving at most 10 arms per second once the backoff
 * engages. That is unmeasurable CPU, while bounding the extra latency on the first frame
 * after an idle period to 100 ms, which is well inside the timeouts of anything that would
 * be waiting for it (ARP retransmits at 1 s, TCP SYN retry at 1 s). Deliberately a FIXED
 * interval rather than an escalating one: escalation would trade a cost that is already
 * negligible for a worst-case latency that is not obvious from the code.
 *
 * SMSC95XX_RX_BACKOFF_LEEWAY_NS -- 50 ms of slack the system may use to coalesce this timer
 * with others, since nothing about a 100 ms idle poll needs to be punctual.
 *
 * WHAT HAPPENS TO A REAL FRAME THAT ARRIVES DURING THE BACKOFF: it is not lost. The chip
 * buffers received frames in its own RX FIFO and hands them over on the next bulk IN
 * transfer, so the frame is delivered up to one backoff interval late. That completion
 * carries bytes, which resets the run to zero and returns the path to immediate re-arming,
 * so the degraded window is at most a single interval and never persists. NOT FULLY
 * VERIFIED, and stated as the honest limit of what is known: the FIFO is finite, so a burst
 * arriving entirely inside one 100 ms window could in principle overflow it and lose
 * frames. That has not been observed and cannot be provoked without hardware. The interface
 * is never left permanently deaf, which is the property that matters most here. */
#define SMSC95XX_RX_IDLE_RUN_LIMIT     8
#define SMSC95XX_RX_BACKOFF_NS         (100ull * 1000ull * 1000ull)
#define SMSC95XX_RX_BACKOFF_LEEWAY_NS  (50ull * 1000ull * 1000ull)

/* While backed off, say so once every this many empty completions rather than on every one.
 * The whole point of the fix is that a quiet device costs nothing, and a log line ten times
 * a second is a cost; at 50 that is one line roughly every five seconds. The Log-level
 * "backing off" line is emitted exactly once per episode regardless. */
#define SMSC95XX_RX_BACKOFF_LOG_EVERY  50

static kern_return_t
allocBuffer(uint64_t len, IOBufferMemoryDescriptor **buf, uint8_t **bytes)
{
    IOAddressSegment range = {};
    kern_return_t    ret;

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, len, 0, buf);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    /* GetAddressRange, not Map: Map creates an IOMemoryMap and returns only the address,
     * leaving no handle to release at teardown. */
    ret = (*buf)->GetAddressRange(&range);
    if (ret != kIOReturnSuccess) {
        OSSafeReleaseNULL(*buf);
        return ret;
    }
    /* Create() is not documented to round down, but the whole point of these buffers is
     * that a transfer length we chose is written into them, so check rather than assume:
     * a short segment would turn every transfer into an overrun. */
    if (range.length < len) {
        OSSafeReleaseNULL(*buf);
        return kIOReturnNoMemory;
    }
    *bytes = reinterpret_cast<uint8_t *>(range.address);
    return kIOReturnSuccess;
}

kern_return_t
SMSC95xxDriver::setupDatapath(void)
{
    kern_return_t ret;

    ret = ivars->interface->CopyPipe(SMSC95XX_EP_BULK_IN, &ivars->pipeIn);
    if (ret != kIOReturnSuccess || ivars->pipeIn == nullptr) {
        Log("CopyPipe(bulk IN 0x%02x) failed: 0x%x", SMSC95XX_EP_BULK_IN, ret);
        return (ret == kIOReturnSuccess) ? kIOReturnNoDevice : ret;
    }
    ret = ivars->interface->CopyPipe(SMSC95XX_EP_BULK_OUT, &ivars->pipeOut);
    if (ret != kIOReturnSuccess || ivars->pipeOut == nullptr) {
        Log("CopyPipe(bulk OUT 0x%02x) failed: 0x%x", SMSC95XX_EP_BULK_OUT, ret);
        return (ret == kIOReturnSuccess) ? kIOReturnNoDevice : ret;
    }

    ret = allocBuffer(SMSC95XX_RX_BUFFER_LEN, &ivars->rxBuffer, &ivars->rxBytes);
    if (ret != kIOReturnSuccess) {
        Log("RX buffer allocation failed: 0x%x", ret);
        return ret;
    }
    ret = allocBuffer(SMSC95XX_TX_BUFFER_LEN, &ivars->txBuffer, &ivars->txBytes);
    if (ret != kIOReturnSuccess) {
        Log("TX buffer allocation failed: 0x%x", ret);
        return ret;
    }

    ret = CreateActionRxComplete(0, &ivars->rxAction);
    if (ret != kIOReturnSuccess) {
        Log("CreateActionRxComplete failed: 0x%x", ret);
        return ret;
    }
    ret = CreateActionTxComplete(0, &ivars->txAction);
    if (ret != kIOReturnSuccess) {
        Log("CreateActionTxComplete failed: 0x%x", ret);
        return ret;
    }

    /* The TX doorbell. Create() gave the submission queue a shared-memory data queue backed
     * by ivars->dispatchQueue; CopyDataQueue hands us that same source (Copy, so it comes
     * retained and teardownDatapath releases it), and SetDataAvailableHandler asks to be
     * called when the stack makes it non-empty. This is the mechanism every shipping Apple
     * network dext uses -- AppleUserECM, the USB Ethernet one, included -- and unlike a
     * withPool DequeueAction it says where the callback runs: "the DataAvailable handler is
     * invoked on the queue set for the target method of the OSAction", i.e. the same queue
     * as RxComplete/TxComplete, so the datapath keeps one serialisation context. */
    ret = ivars->txSubmit->CopyDataQueue(&ivars->txDataQueue);
    if (ret != kIOReturnSuccess || ivars->txDataQueue == nullptr) {
        Log("CopyDataQueue(TX submission) failed: 0x%x", ret);
        return (ret == kIOReturnSuccess) ? kIOReturnNoResources : ret;
    }
    ret = CreateActionTxDataAvailable(0, &ivars->txDataAvailableAction);
    if (ret != kIOReturnSuccess) {
        Log("CreateActionTxDataAvailable failed: 0x%x", ret);
        return ret;
    }
    ret = ivars->txDataQueue->SetDataAvailableHandler(ivars->txDataAvailableAction);
    if (ret != kIOReturnSuccess) {
        Log("SetDataAvailableHandler(TX submission) failed: 0x%x", ret);
        return ret;
    }

    /* The idle-RX backoff timer, on the SAME dispatch queue as everything else, so its
     * handler serialises with RxComplete and needs no locking -- the queue is ivars->
     * dispatchQueue, which by this point is the driver's own Default queue.
     *
     * A failure here fails setup rather than degrading quietly. Without the timer the only
     * two available behaviours are the hot loop this exists to prevent and a receive path
     * that stops for good, and neither is something to fall back to silently. */
    ret = IOTimerDispatchSource::Create(ivars->dispatchQueue, &ivars->rxBackoffTimer);
    if (ret != kIOReturnSuccess || ivars->rxBackoffTimer == nullptr) {
        Log("IOTimerDispatchSource::Create(RX backoff) failed: 0x%x", ret);
        return (ret == kIOReturnSuccess) ? kIOReturnNoResources : ret;
    }
    ret = CreateActionRxBackoffExpired(0, &ivars->rxBackoffAction);
    if (ret != kIOReturnSuccess) {
        Log("CreateActionRxBackoffExpired failed: 0x%x", ret);
        return ret;
    }
    ret = ivars->rxBackoffTimer->SetHandler(ivars->rxBackoffAction);
    if (ret != kIOReturnSuccess) {
        Log("SetHandler(RX backoff timer) failed: 0x%x", ret);
        return ret;
    }
    /* Enabled now, armed later: an enabled timer with no WakeAtTime deadline does not fire,
     * so this costs nothing until a run of empty completions asks for it. */
    ret = ivars->rxBackoffTimer->SetEnable(true);
    if (ret != kIOReturnSuccess) {
        Log("SetEnable(true) on the RX backoff timer failed: 0x%x", ret);
        return ret;
    }

    /* LAST, after every object the transmit path dereferences exists: this is the flag that
     * lets the drain loop consume packets, and teardownDatapath clears it before it aborts
     * anything. Setting it earlier would open a window in which a doorbell could hand a
     * packet to a pipe that setup had not finished acquiring. */
    ivars->txDatapathReady = true;

    Log("datapath ready: pipes 0x%02x/0x%02x, rx buffer %u bytes, tx buffer %u bytes "
        "(%u-byte header + %u-byte frame), one transfer in flight per direction, "
        "idle-RX backoff after %u empty completions then every %llu ms",
        SMSC95XX_EP_BULK_IN, SMSC95XX_EP_BULK_OUT, SMSC95XX_RX_BUFFER_LEN,
        (unsigned int)SMSC95XX_TX_BUFFER_LEN, (unsigned int)SMSC95XX_TX_HEADER_LEN,
        (unsigned int)SMSC95XX_TX_MAX_FRAME, (unsigned int)SMSC95XX_RX_IDLE_RUN_LIMIT,
        SMSC95XX_RX_BACKOFF_NS / 1000000ull);
    return kIOReturnSuccess;
}

/* The permanent datapath totals. Two lines, TX then RX, rather than one unreadable one.
 *
 * This is NOT part of the DIAG6/DIAG7 tracing and must outlive it: the trace answers "what
 * happened to this frame", which is only useful while a bug is being chased, whereas these two
 * lines answer "did the interface move traffic, and did it lose anything", which is the
 * question after every hardware run and the only one `log show` can still answer once the
 * per-frame lines have been throttled away.
 *
 * The numbers to read first are the two accounting identities, because a break in either is a
 * leak rather than a slowdown:
 *   TX  dequeued == returned + lost + (1 if a transfer is in flight)
 *   RX  records  == frames + dropped, and frames == enqueued + enqFail
 * fromSubmitQ against submitQEmpty then says which source the receive path is actually using.
 */
void
SMSC95xxDriver::logDatapathCounters(const char *why)
{
    Log("counters at %{public}s -- TX: doorbells %llu drains %llu deferred %llu | dequeued "
        "%llu submitted %llu completions %llu returned %llu lost %llu | frames %llu bytes "
        "%llu | rejected %llu submitFail %llu errors %llu stalls %llu aborted %llu",
        why, ivars->txDoorbells, ivars->txDrains, ivars->txDeferred, ivars->txDequeued,
        ivars->txSubmitted, ivars->txCompletions, ivars->txReturned, ivars->txLost,
        ivars->txFrames, ivars->txBytesSent, ivars->txRejected, ivars->txSubmitFailures,
        ivars->txErrors, ivars->txStalls, ivars->txAborted);

    Log("counters at %{public}s -- RX: completions %llu errors %llu zeroLen %llu arms %llu "
        "armFail %llu | bytes %llu records %llu frames %llu dropped %llu | enqueued %llu "
        "enqFail %llu lost %llu | fromSubmitQ %llu submitQEmpty %llu poolFail %llu",
        why, ivars->rxCompletions, ivars->rxCompletionErrors, ivars->rxZeroLength,
        ivars->rxArmCount, ivars->rxArmFailures, ivars->rxByteCount, ivars->rxRecords,
        ivars->rxFrames, ivars->rxDropped, ivars->rxEnqueued, ivars->rxEnqueueFailures,
        ivars->rxLost, ivars->rxSubmitDequeued, ivars->rxSubmitEmpty,
        ivars->rxPoolFailures);
}

void
SMSC95xxDriver::teardownDatapath(void)
{
    /* Stop the receive loop BEFORE anything is aborted or released, and before the TX
     * doorbell is touched. Two reasons, both about the abort below:
     *
     *  - Abort(kIOUSBAbortSynchronous) does not return until the aborted IO has completed,
     *    and the header notes that new IO is blocked "unless they are submitted from an
     *    aborted IO's completion routine" -- i.e. the aborted completion may be delivered
     *    while this function is still on the stack. RxComplete is what would re-arm, and
     *    with rxRunning already false it will not, so the abort cannot race a fresh AsyncIO
     *    into the buffer this function is about to release.
     *  - RxComplete's aborted branch returns before it looks at ivars->rxBytes, and its
     *    teardown branch checks for the released objects, so either ordering is safe -- but
     *    this makes "no re-arm after teardown starts" true by state rather than by timing.
     *
     * rxArmed is NOT cleared here: the completion for the aborted transfer clears it, and
     * clearing it early would let a late armRxRead() double-arm. */
    /* The totals go through logDatapathCounters so they survive the DIAG7 strip; what stays
     * here as tracing is the datapath STATE, which only matters while teardown ordering is
     * being reasoned about. */
    logDatapathCounters("datapath teardown");
    Diag7("teardownDatapath: entered. rxRunning=%{public}s rxArmed=%{public}s",
          ivars->rxRunning ? "true" : "false", ivars->rxArmed ? "true" : "false");
    Diag7("teardownDatapath: idle-RX backoff state: idleRun=%llu active=%{public}s "
          "episodes=%llu wakes=%llu", ivars->rxIdleRun,
          ivars->rxBackoffActive ? "true" : "false", ivars->rxBackoffEpisodes,
          ivars->rxBackoffWakes);
    ivars->rxRunning = false;

    /* The transmit path is closed to new work BEFORE anything is aborted or released, and
     * for the same reason rxRunning is cleared first. The bulk OUT Abort below is
     * synchronous, and the header is explicit that an aborted transfer's completion may be
     * delivered while the abort is still on the stack -- so TxComplete can run from inside
     * this function. With this flag already false, that completion returns its own packet to
     * the stack (the TX completion queue is still alive: TearDown releases the four queues
     * only after this function returns) and then stops, instead of dequeuing more packets
     * and framing them into a buffer that is about to be released.
     *
     * txInFlight is deliberately NOT cleared here, exactly as rxArmed is not: the aborted
     * transfer's own completion clears it, and clearing it early would let a drain slip a
     * second transfer past the one-in-flight rule during teardown. */
    ivars->txDatapathReady = false;
    Diag6("teardownDatapath: TX state: inFlight=%{public}s packet=%{public}s doorbells=%llu "
          "drains=%llu dequeued=%llu submitted=%llu completions=%llu frames=%llu "
          "returned=%llu lost=%llu bytes=%llu rejected=%llu submitFail=%llu stalls=%llu "
          "aborted=%llu deferred=%llu errors=%llu",
          ivars->txInFlight ? "true" : "false",
          ivars->txPacket != nullptr ? "set (its completion has not run yet)" : "null",
          ivars->txDoorbells, ivars->txDrains, ivars->txDequeued, ivars->txSubmitted,
          ivars->txCompletions, ivars->txFrames, ivars->txReturned, ivars->txLost,
          ivars->txBytesSent, ivars->txRejected, ivars->txSubmitFailures, ivars->txStalls,
          ivars->txAborted, ivars->txDeferred, ivars->txErrors);

    /* The backoff timer goes down FIRST, for the same reason rxRunning is cleared first: a
     * pending wake would call armRxRead() against the buffer this function is about to
     * release.
     *
     * SetEnable(false) ONLY. Do NOT call Cancel() here.
     *
     * This function used to call `Cancel(^{ })`, on the reasoning that the source is ours
     * and the SDK says a cancelled source can only be freed. That crashed the driver:
     *
     *   EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0000000000000008
     *     invocation function for block in IOTimerDispatchSource::Cancel_Impl(...)
     *     _dispatch_source_cancel_callout
     *
     * `^{ }` is a STACK block. Cancel stores it and invokes it asynchronously, by which
     * time this function has returned and that stack frame is gone, so the cancel callout
     * dereferences a dead block descriptor -- hence the fault at a tiny offset.
     *
     * The cost of getting this wrong was not a single crash. The driver crashed on EVERY
     * received frame, IOKit relaunched it through xpcproxy each time, and the respawn loop
     * exhausted SPTM shared regions and PANICKED THE MACHINE:
     *   panic ... [SPTM] VIOLATION_SHARED_REGIONS_EXHAUSTED: shared_region_alloc
     * with xpcproxy as the panicking task. A userspace fault in a dext becomes a system
     * panic if it can be made to repeat quickly enough, which is worth remembering before
     * treating any dext crash as merely a restart.
     *
     * SetEnable(false) stops the source firing, and releasing our reference below is then
     * all that is ours to do. This matches what the txDataQueue teardown a few lines down
     * already does deliberately, and that path never crashed. If a cancel handler is ever
     * genuinely needed here, it must be a heap block whose lifetime outlives this frame,
     * and the source must not be released until it has run. */
    if (ivars->rxBackoffTimer != nullptr) {
        kern_return_t er = ivars->rxBackoffTimer->SetEnable(false);
        Diag7("teardownDatapath: RX backoff timer SetEnable(false) -> 0x%x "
              "(no Cancel: see the comment above)", er);
    }
    OSSafeReleaseNULL(ivars->rxBackoffAction);
    OSSafeReleaseNULL(ivars->rxBackoffTimer);
    ivars->rxBackoffActive = false;
    ivars->rxIdleRun       = 0;

    /* The doorbell goes next, and before anything is released: TxDataAvailable
     * dereferences ivars->txSubmit and ivars->txComplete, and TearDown releases both queues
     * the moment this function returns. SetEnable(false) stops further DataAvailable
     * callbacks; a handler already queued behind us still cannot run early, because it is
     * dispatched on the same queue as this teardown, and by the time it does run the null
     * guards at the top of TxDataAvailable see the released queues and return.
     * Not Cancel(): the source belongs to the submission queue, which created it and frees
     * it with itself, so cancelling it here would be tearing down someone else's object.
     * Releasing our CopyDataQueue reference and our action reference is all that is ours to
     * do -- the source drops its own retain on the action when the queue frees it. */
    if (ivars->txDataQueue != nullptr) {
        ivars->txDataQueue->SetEnable(false);
    }
    OSSafeReleaseNULL(ivars->txDataAvailableAction);
    OSSafeReleaseNULL(ivars->txDataQueue);

    /* Abort before releasing, and abort SYNCHRONOUSLY: a transfer still in flight against
     * a released buffer is a use-after-free inside the USB stack. kIOUSBAbortSynchronous
     * does not return until the aborted IO has completed, which is the guarantee the
     * releases below depend on. */
    if (ivars->pipeIn != nullptr) {
        kern_return_t ar = ivars->pipeIn->Abort(kIOUSBAbortSynchronous, kIOReturnAborted,
                                                this);
        Diag7("teardownDatapath: Abort(bulk IN, synchronous) -> 0x%x; rxArmed is now "
              "%{public}s", ar, ivars->rxArmed ? "true (completion not seen yet)" : "false");
    }
    if (ivars->pipeOut != nullptr) {
        kern_return_t ar = ivars->pipeOut->Abort(kIOUSBAbortSynchronous, kIOReturnAborted,
                                                 this);
        Diag7("teardownDatapath: Abort(bulk OUT, synchronous) -> 0x%x", ar);
    }
    /* Actions before pipes: an action is what a completion is delivered through, and
     * nothing can still be pending once the aborts above have returned. */
    OSSafeReleaseNULL(ivars->rxAction);
    OSSafeReleaseNULL(ivars->txAction);
    /* Retained by CopyPipe ("the caller must release the IOUSBHostPipe when finished"),
     * so these are ours to release -- unlike ivars->interface, which is borrowed. */
    OSSafeReleaseNULL(ivars->pipeIn);
    OSSafeReleaseNULL(ivars->pipeOut);
    /* The mapped addresses belong to the descriptors; drop them first so nothing can read
     * a pointer into freed memory. */
    ivars->rxBytes = nullptr;
    ivars->txBytes = nullptr;
    OSSafeReleaseNULL(ivars->rxBuffer);
    OSSafeReleaseNULL(ivars->txBuffer);

    /* Both pipes have been aborted synchronously, so every completion has run and this
     * should already be false with no packet held. If it is not, the one-in-flight
     * bookkeeping is broken somewhere and the packet cannot safely be completed now: a
     * completion that has not been delivered yet still might be, and completing the same
     * packet twice hands the stack a packet we also hold a pointer to. Dropping it costs
     * nothing here in particular -- TearDown releases the whole 64-packet pool a few lines
     * later, which reclaims it -- so the honest action is to account for it and say so. */
    if (ivars->txPacket != nullptr || ivars->txInFlight) {
        ivars->txLost++;
        Log("teardownDatapath: a TX was STILL in flight after a synchronous abort "
            "(inFlight=%{public}s packet=%{public}s) -- the packet is dropped rather than "
            "risk completing it twice (total lost %llu). The one-in-flight bookkeeping is "
            "wrong if this line ever appears.",
            ivars->txInFlight ? "true" : "false",
            ivars->txPacket != nullptr ? "set" : "null", ivars->txLost);
    }
    ivars->txPacket   = nullptr;
    ivars->txInFlight = false;
    /* Only now, with the pipe released and the buffer gone, is it certain that no transfer
     * can be outstanding -- so this is where rxArmed stops being a claim about the hardware
     * and becomes just a cleared flag. */
    ivars->rxArmed = false;
    Diag7("teardownDatapath: complete; pipes, buffers and actions released");
}

/* Submit one bulk IN transfer into ivars->rxBuffer.
 *
 * IDEMPOTENT by design: if a transfer is already outstanding this returns success without
 * submitting a second one. There is exactly one RX buffer, so two overlapping AsyncIOs would
 * have the device writing the same memory twice; the postcondition callers care about ("a
 * read is outstanding") already holds, so refusing quietly is the honest answer rather than
 * an error the caller would have to special-case. */
kern_return_t
SMSC95xxDriver::armRxRead(void)
{
    if (ivars->pipeIn == nullptr || ivars->rxAction == nullptr
            || ivars->rxBuffer == nullptr) {
        /* Reached if something arms after teardownDatapath has run. Not an error worth
         * failing an unrelated caller over, but it must never be silent. */
        Log("armRxRead: datapath is gone (pipeIn=%{public}s rxAction=%{public}s "
            "rxBuffer=%{public}s) -- not arming",
            ivars->pipeIn   != nullptr ? "set" : "null",
            ivars->rxAction != nullptr ? "set" : "null",
            ivars->rxBuffer != nullptr ? "set" : "null");
        return kIOReturnNotAttached;
    }
    if (ivars->rxArmed) {
        Diag7("armRxRead: a transfer is already outstanding -- not double-arming "
              "(arms=%llu, completions=%llu)", ivars->rxArmCount, ivars->rxCompletions);
        return kIOReturnSuccess;
    }

    kern_return_t ret = ivars->pipeIn->AsyncIO(ivars->rxBuffer, SMSC95XX_RX_BUFFER_LEN,
                                               ivars->rxAction, 0);
    if (ret != kIOReturnSuccess) {
        ivars->rxArmFailures++;
        Log("AsyncIO(bulk IN) failed: 0x%x (arm failures %llu)", ret, ivars->rxArmFailures);
        return ret;
    }
    ivars->rxArmed = true;
    ivars->rxArmCount++;
    Diag7("armRxRead: AsyncIO(bulk IN 0x%02x, %u bytes) armed -> 0x%x (arm #%llu, "
          "rxRunning=%{public}s)", SMSC95XX_EP_BULK_IN, SMSC95XX_RX_BUFFER_LEN, ret,
          ivars->rxArmCount, ivars->rxRunning ? "true" : "false");
    return kIOReturnSuccess;
}

/* The single re-arm decision point for RxComplete, and the whole of the idle-RX backoff.
 *
 * `carriedData` is the only input: a completion that delivered bytes proves the device has
 * something to say, so the receive path returns to (or stays at) immediate re-arming. A
 * completion that delivered nothing -- a zero-length success or a failed transfer -- can
 * repeat instantly, so a run of them is what the backoff counts.
 *
 * Runs on the driver's dispatch queue, like every other datapath callback, so ivars->
 * rxIdleRun needs no locking. */
void
SMSC95xxDriver::rearmRxRead(bool carriedData)
{
    if (carriedData) {
        if (ivars->rxBackoffActive || ivars->rxIdleRun > 0) {
            /* Once per episode, not per completion: the un-guarded loop's log flood was
             * itself part of the cost being fixed. */
            Log("RxComplete: traffic resumed after %llu empty completions%{public}s -- back "
                "to immediate re-arming", ivars->rxIdleRun,
                ivars->rxBackoffActive ? " and a backoff" : "");
        }
        ivars->rxIdleRun       = 0;
        ivars->rxBackoffActive = false;
        kern_return_t aret = armRxRead();
        Diag7("RxComplete: re-armed immediately -> 0x%x", aret);
        if (aret != kIOReturnSuccess) {
            Log("RxComplete: FAILED to re-arm the bulk IN read: 0x%x -- the receive path is "
                "now dead until the interface is re-enabled", aret);
        }
        return;
    }

    ivars->rxIdleRun++;

    if (ivars->rxIdleRun <= SMSC95XX_RX_IDLE_RUN_LIMIT) {
        kern_return_t aret = armRxRead();
        Diag7("RxComplete: re-armed immediately after an empty completion (%llu of %u "
              "before backoff) -> 0x%x", ivars->rxIdleRun,
              (unsigned int)SMSC95XX_RX_IDLE_RUN_LIMIT, aret);
        if (aret != kIOReturnSuccess) {
            Log("RxComplete: FAILED to re-arm the bulk IN read: 0x%x -- the receive path is "
                "now dead until the interface is re-enabled", aret);
        }
        return;
    }

    /* Past the threshold: do NOT re-arm now. Hand the re-arm to the timer instead, which
     * turns an unbounded spin into one transfer every SMSC95XX_RX_BACKOFF_NS. */
    if (ivars->rxBackoffTimer == nullptr) {
        /* Only reachable if teardown released the timer while a completion was in flight,
         * which rxRunning should already have caught. Stop rather than spin: the buffer is
         * being released anyway. */
        Log("RxComplete: %llu consecutive empty completions and no backoff timer "
            "(teardown in progress?) -- not re-arming", ivars->rxIdleRun);
        return;
    }

    uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + SMSC95XX_RX_BACKOFF_NS;
    kern_return_t tret = ivars->rxBackoffTimer->WakeAtTime(kIOTimerClockUptimeRaw, deadline,
                                                          SMSC95XX_RX_BACKOFF_LEEWAY_NS);
    if (tret != kIOReturnSuccess) {
        /* The timer refused the deadline, so nothing will re-arm this read. Re-arming
         * immediately would restore the busy loop; stopping leaves the interface deaf until
         * it is disabled and re-enabled. Deaf-and-loud is the lesser evil, and this is
         * logged unconditionally because it is the one path that needs the user to act. */
        Log("RxComplete: WakeAtTime for the RX backoff FAILED: 0x%x -- the receive path is "
            "now stopped after %llu empty completions; disable and re-enable the interface "
            "to restart it", tret, ivars->rxIdleRun);
        ivars->rxBackoffActive = false;
        return;
    }

    if (!ivars->rxBackoffActive) {
        /* ONCE per episode. This is the line that says the busy loop was caught. */
        ivars->rxBackoffActive = true;
        ivars->rxBackoffEpisodes++;
        Log("RxComplete: %llu consecutive completions delivered no data -- backing off, now "
            "re-arming every %llu ms instead of immediately (episode #%llu). A frame arriving "
            "meanwhile waits in the chip's RX FIFO and is delivered on the next transfer, "
            "which resets this.", ivars->rxIdleRun, SMSC95XX_RX_BACKOFF_NS / 1000000ull,
            ivars->rxBackoffEpisodes);
    } else if ((ivars->rxIdleRun % SMSC95XX_RX_BACKOFF_LOG_EVERY) == 0) {
        /* Throttled, not per iteration: still idle, still backed off, nothing new to say. */
        Diag7("RxComplete: still idle (%llu empty completions, %llu timer wakes) -- next "
              "re-arm in %llu ms", ivars->rxIdleRun, ivars->rxBackoffWakes,
              SMSC95XX_RX_BACKOFF_NS / 1000000ull);
    }
}

/* The backoff timer fired: re-arm the read that rearmRxRead deliberately did not.
 *
 * Delivered on the driver's dispatch queue, so it cannot overlap RxComplete. If the read is
 * still empty when it completes, rearmRxRead schedules this again -- so the steady state
 * for an idle device is one transfer per backoff interval, and the receive path stays live
 * without spinning. */
void
IMPL(SMSC95xxDriver, RxBackoffExpired)
{
    (void)action;
    (void)time;

    ivars->rxBackoffWakes++;

    if (!ivars->rxRunning) {
        Diag7("RxBackoffExpired #%llu: RX is not running -- not arming",
              ivars->rxBackoffWakes);
        return;
    }

    kern_return_t aret = armRxRead();
    /* Throttled for the same reason as the "still idle" line: the first wake of an episode is
     * the interesting one, and after that this fires ten times a second forever. A failure is
     * always logged, below. */
    if (ivars->rxBackoffWakes == 1
            || (ivars->rxBackoffWakes % SMSC95XX_RX_BACKOFF_LOG_EVERY) == 0) {
        Diag7("RxBackoffExpired #%llu: armRxRead -> 0x%x (idleRun=%llu)",
              ivars->rxBackoffWakes, aret, ivars->rxIdleRun);
    }
    if (aret != kIOReturnSuccess) {
        Log("RxBackoffExpired: failed to re-arm the bulk IN read: 0x%x -- the receive path "
            "is now dead until the interface is re-enabled", aret);
    }
}

/* ---- transmit ------------------------------------------------------------------------
 *
 * THE ACCOUNTING RULE THE WHOLE TRANSMIT PATH IS BUILT AROUND: every packet that comes out
 * of DequeuePackets is handed back to the stack EXACTLY ONCE, on every path -- transmitted,
 * refused for its length, refused by the framing, refused by the pipe, or aborted at
 * teardown. A packet consumed and never completed is gone from the 64-packet pool for good,
 * so 64 of them wedge transmit with no error anywhere; a packet completed twice is worse,
 * because the stack then owns a packet this driver still holds a pointer to.
 *
 * Two things make that rule auditable rather than merely intended:
 *
 *  - txReturnPacket below is the ONLY place a packet goes back, so "exactly once" is a
 *    property of its call sites, which all fit in one screen, instead of a property of the
 *    whole file.
 *  - the drain loop takes ONE packet at a time and disposes of it before taking another, so
 *    there is never a batch of packets held in a local array with an early return able to
 *    strand it. A batch is how a partially-handled group leaks.
 */

/* Hand ONE packet back to the network stack. The single exit for a consumed packet.
 *
 * `why` names the path taken, so the log distinguishes a transmitted packet from a refused
 * one without having to correlate timestamps. A null packet is accepted and ignored, which is
 * what lets callers pass ivars->txPacket unconditionally. */
static void
txReturnPacket(SMSC95xxDriver_IVars *ivars, IOUserNetworkPacket *packet, const char *why)
{
    if (packet == nullptr) {
        return;
    }
    if (ivars->txComplete == nullptr) {
        /* Only reachable if the completion queue was released while a packet was held, which
         * teardown ordering is meant to prevent (TearDown releases the queues only after
         * teardownDatapath has aborted both pipes and every completion has run). The packet
         * is reclaimed when the pool goes, but it never reached the stack, so count it. */
        ivars->txLost++;
        Log("TX: cannot complete a packet (%{public}s): the TX completion queue is gone -- "
            "1 packet LOST (total lost %llu)", why, ivars->txLost);
        return;
    }

    IOReturn ret = ivars->txComplete->enqueuePackets(&packet, 1);
    if (ret != kIOReturnSuccess) {
        /* One packet, one call: unlike the RX batch there is no partial-success ambiguity to
         * reason about. It is out of the submission queue and cannot be put back, so it is
         * lost -- said loudly, because silent loss surfaces much later as a pool that never
         * refills and looks nothing like this. */
        ivars->txLost++;
        Log("TX: enqueuePackets(1) on the TX completion queue FAILED: 0x%x (%{public}s) -- "
            "1 packet LOST (total lost %llu)", ret, why, ivars->txLost);
        return;
    }

    ivars->txReturned++;
    Diag6("TX: packet returned to the stack (%{public}s): dequeued=%llu returned=%llu "
          "lost=%llu inFlight=%{public}s", why, ivars->txDequeued, ivars->txReturned,
          ivars->txLost, ivars->txInFlight ? "true" : "false");
}

/* Frame one packet into the TX scratch buffer and submit it on the bulk OUT pipe.
 *
 * Returns kIOReturnSuccess only if the transfer was accepted, in which case the packet now
 * belongs to the in-flight transfer (ivars->txPacket) and TxComplete will return it. On any
 * failure the packet is untouched and still the CALLER's to return -- that split is
 * deliberate, so this function never has to know how to complete a packet and the caller
 * never has to guess whether it still owns one.
 *
 * The caller must have checked ivars->txInFlight: there is one scratch buffer, so a second
 * overlapping transfer would frame over bytes the first one is still sending. */
static kern_return_t
txSubmitPacket(SMSC95xxDriver_IVars *ivars, IOUserNetworkPacket *packet)
{
    uint32_t frameLen = packet->getDataLength();

    /* WHERE THE FRAME ACTUALLY STARTS, and the bug this used to be.
     *
     * getDataVirtualAddress() returns the base of the packet's buffer. It does NOT add the
     * packet's data offset -- in the DriverKit 25.5 NetworkingDriverKit it is three
     * instructions, `ldr x8, [x0, #0x30]; ldr x0, [x8, #0x10]; ret`, a bare ivar load, and
     * getDataOff() reads a different ivar entirely. The frame begins at base + getDataOff().
     *
     * getTxDataOffset() returns SMSC95XX_TX_HEADER_LEN, so the family reserves 8 bytes of
     * headroom on every TX packet and getDataOff() is 8. Copying frameLen bytes from the base
     * therefore sent 8 bytes of reserved headroom followed by only the first frameLen - 8
     * bytes of the frame. On the wire that looked like eight leading zero bytes and a frame
     * truncated by eight at the tail -- which was misread as the chip failing to strip our TX
     * command header. It strips it correctly; the source pointer was wrong.
     *
     * Adding getDataOff() is right whatever getTxDataOffset() returns, which is why the
     * override stays at 8 rather than reverting to the family's default of 0: a non-zero
     * offset keeps this arithmetic exercised instead of adding zero on every frame.
     *
     * Note what this build does NOT do with the headroom, and why: even with 8 writable bytes
     * in front of the frame, transmitting in place is not possible through AsyncIO, which
     * takes an IOMemoryDescriptor and a length and NO offset. The packet's bytes live
     * somewhere inside the pool's memory, so a zero-copy transmit needs a descriptor covering
     * exactly [frame - 8, frame + len), i.e. pool->CopyMemoryDescriptor() once plus an
     * IOMemoryDescriptor::CreateSubMemoryDescriptor per packet -- a kernel round trip and an
     * object allocation on every frame, against a memcpy of at most 1518 bytes. That is a
     * throughput question for M6 (AsyncIOBundled with a ring of pre-made sub-descriptors),
     * not a correctness one for M5, so the copy into the persistent scratch buffer stays: it
     * is known-safe and its cost is a memcpy the CPU does in well under a microsecond. */
    size_t   dataOff = packet->getDataOff();
    uint64_t frameAt = packet->getDataVirtualAddress() + dataOff;

    if (ivars->txDequeued <= SMSC95XX_TX_TRACE_PACKETS) {
        Diag6("TX framing probe (packet %llu of the first %u): buffer base 0x%llx + "
              "getDataOff %zu (getTxDataOffset asked for %u) = frame at 0x%llx, "
              "dataLength=%u, linkHeaderLength=%u",
              ivars->txDequeued, (unsigned int)SMSC95XX_TX_TRACE_PACKETS,
              packet->getDataVirtualAddress(), dataOff,
              (unsigned int)SMSC95XX_TX_HEADER_LEN, frameAt, frameLen,
              (unsigned int)packet->getLinkHeaderLength());
    }

    /* THE LENGTH GUARD. Kept as a guard, not as an open question: the offload investigation
     * is closed (see SMSC95XX_TX_MAX_FRAME). A length outside the plausible range is refused
     * rather than memcpy'd, because the copy below is only as safe as this number. */
    if (frameLen == 0 || frameLen > SMSC95XX_TX_MAX_FRAME) {
        ivars->txRejected++;
        Log("TX: REFUSING implausible length %u (0 or over %u) -- not a framing fault "
            "(rejected %llu)", frameLen, (unsigned int)SMSC95XX_TX_MAX_FRAME,
            ivars->txRejected);
        return kIOReturnBadArgument;
    }

    if (ivars->txBytes == nullptr || ivars->txBuffer == nullptr || ivars->pipeOut == nullptr
            || ivars->txAction == nullptr) {
        /* txDatapathReady should already have kept the drain loop out of here. */
        ivars->txRejected++;
        Log("TX: the datapath is gone (txBytes=%{public}s txBuffer=%{public}s "
            "pipeOut=%{public}s txAction=%{public}s) -- refusing a %u-byte frame",
            ivars->txBytes  != nullptr ? "set" : "null",
            ivars->txBuffer != nullptr ? "set" : "null",
            ivars->pipeOut  != nullptr ? "set" : "null",
            ivars->txAction != nullptr ? "set" : "null", frameLen);
        return kIOReturnNotAttached;
    }

    /* The 8-byte TX command header must physically precede the frame, so build both in the
     * scratch buffer: header first, then the frame after it. smsc95xx_tx_prepend applies the
     * protocol's own length limits (SMSC95XX_FRAME_MIN..SMSC95XX_FRAME_MAX) and returns 0
     * rather than truncating, which is what keeps a bad length from becoming a malformed
     * transfer the chip would interpret as something else entirely. */
    size_t hdrLen = smsc95xx_tx_prepend(ivars->txBytes, SMSC95XX_TX_BUFFER_LEN, frameLen);
    if (hdrLen != SMSC95XX_TX_HEADER_LEN) {
        ivars->txRejected++;
        Log("TX: smsc95xx_tx_prepend REFUSED frame_len %u (outside %u..%u) -> %zu "
            "(rejected %llu)", frameLen, (unsigned int)SMSC95XX_FRAME_MIN,
            (unsigned int)SMSC95XX_FRAME_MAX, hdrLen, ivars->txRejected);
        return kIOReturnBadArgument;
    }

    /* The same safety net the receive path carries, for the same reason and in the opposite
     * direction: a fault here is not one crash but a per-frame crash, and IOKit's respawn loop
     * turns that into a kernel panic (see the RX guard in RxComplete for the full account).
     * Reading a bad address is as fatal as writing one, so refuse rather than memcpy. */
    if (frameAt < 0x100000ull || dataOff + frameLen > SMSC95XX_POOL_BUFFER_SIZE) {
        ivars->txRejected++;
        Log("TX: REFUSING to copy: frame address 0x%llx = base 0x%llx + offset %zu, length %u, "
            "against a %u-byte packet buffer -- either the address is not mapped or the offset "
            "and length do not fit, and reading past a buffer would fault and, via the dext "
            "respawn loop, panic the kernel (rejected %llu)",
            frameAt, packet->getDataVirtualAddress(), dataOff, frameLen,
            (unsigned int)SMSC95XX_POOL_BUFFER_SIZE, ivars->txRejected);
        return kIOReturnBadArgument;
    }

    memcpy(ivars->txBytes + hdrLen, reinterpret_cast<const void *>(frameAt), frameLen);

    uint32_t xferLen = (uint32_t)(hdrLen + frameLen);

    /* Set BEFORE AsyncIO, not after: the completion is delivered on this same dispatch queue
     * so it cannot run while this function is on the stack -- but ordering the state first
     * means there is no window in which the flags disagree with reality, and it is the
     * failure branch below that has to undo them rather than the success path that has to
     * remember to set them. */
    ivars->txInFlight = true;
    ivars->txPacket   = packet;

    /* completionTimeoutMs 0, the same as the bulk IN arm. A bulk transfer that is never
     * completed would leave txInFlight set forever and wedge transmit, but the USB stack
     * completes every accepted transfer one way or another -- with an error on device
     * removal, with kIOReturnAborted on Abort -- so a timeout would only be insurance
     * against a stack bug, at the price of turning a slow device into dropped frames. */
    kern_return_t ret = ivars->pipeOut->AsyncIO(ivars->txBuffer, xferLen, ivars->txAction, 0);
    if (ret != kIOReturnSuccess) {
        ivars->txInFlight = false;
        ivars->txPacket   = nullptr;
        ivars->txSubmitFailures++;
        Log("TX: AsyncIO(bulk OUT 0x%02x, %u bytes) FAILED: 0x%x (submit failures %llu) -- "
            "the packet goes back to the stack undelivered", SMSC95XX_EP_BULK_OUT, xferLen,
            ret, ivars->txSubmitFailures);
        return ret;
    }

    ivars->txSubmitted++;
    Diag6("TX: submitted transfer #%llu on bulk OUT 0x%02x: %u bytes = %zu-byte header "
          "(cmd_a 0x%02x%02x%02x%02x cmd_b 0x%02x%02x%02x%02x) + %u-byte frame",
          ivars->txSubmitted, SMSC95XX_EP_BULK_OUT, xferLen, hdrLen,
          ivars->txBytes[3], ivars->txBytes[2], ivars->txBytes[1], ivars->txBytes[0],
          ivars->txBytes[7], ivars->txBytes[6], ivars->txBytes[5], ivars->txBytes[4],
          frameLen);
    return kIOReturnSuccess;
}

/* Consume the TX submission queue: transmit what can be transmitted now, complete every
 * packet taken, and leave the rest queued.
 *
 * Called from TWO places, and both are necessary:
 *  - TxDataAvailable, the doorbell, when the stack makes the queue non-empty;
 *  - TxComplete, because the doorbell is EDGE-triggered. A packet the stack enqueued while a
 *    transfer was in flight has already had its notification spent, so if the completion did
 *    not pull it, it would sit in the queue until the next unrelated enqueue happened to ring
 *    the bell again -- which looks exactly like a stall.
 *
 * ONE packet per DequeuePackets call, not a batch. With one transfer in flight there is
 * nothing to do with a second packet except hold it, and a held packet is precisely what the
 * accounting rule above is trying to avoid; taking them one at a time means the only packet
 * this function is ever responsible for is the one in `packet`. Backpressure is applied by
 * simply not consuming: the packets stay in the submission queue, which is the queue's own
 * mechanism, so nothing is dropped and nothing needs to be requeued.
 *
 * requestDequeue() is deliberately NOT used to ask for more. The submission queue was built
 * with the dispatch-queue overload of Create, so it has no DequeueAction for requestDequeue
 * to call; and this runs on the driver's dispatch queue in both callers, which is where a
 * dequeue is allowed to happen, so calling the drain directly is both simpler and free of
 * assumptions about what requestDequeue does on a queue with no dequeue handler. */
static void
txDrainSubmission(SMSC95xxDriver_IVars *ivars, const char *why)
{
    ivars->txDrains++;

    if (ivars->txSubmit == nullptr || ivars->txComplete == nullptr) {
        Log("TX drain (%{public}s): the packet queues are gone (txSubmit=%{public}s "
            "txComplete=%{public}s) -- nothing consumed", why,
            ivars->txSubmit   != nullptr ? "set" : "null",
            ivars->txComplete != nullptr ? "set" : "null");
        return;
    }
    /* The teardown gate. See the ivar comment: teardownDatapath clears this before it aborts
     * the bulk OUT pipe, and the aborted transfer's completion can call this from inside that
     * abort. Consuming a packet here would frame it into a buffer that is about to be
     * released, and there would be no completion left to hand it back. */
    if (!ivars->txDatapathReady) {
        Diag6("TX drain (%{public}s): the datapath is being torn down -- not consuming "
              "(anything still queued is the stack's, and is never taken from it)", why);
        return;
    }

    uint32_t taken = 0;
    uint32_t sent  = 0;
    uint32_t failed = 0;

    /* Bounded without needing a counter: each iteration either submits (which sets
     * txInFlight and ends the loop) or disposes of one packet, and the pool holds 64 packets
     * in total, so the stack cannot present more than 64 without first getting some back. */
    while (!ivars->txInFlight) {
        IOUserNetworkPacket *packet = nullptr;
        uint32_t             count  = ivars->txSubmit->DequeuePackets(&packet, 1);

        if (count == 0) {
            break;                              /* the queue is empty: the normal exit */
        }
        if (packet == nullptr) {
            /* A count with no packet. Nothing to account for and nothing to retry, but
             * continuing could spin, so stop and say so. */
            ivars->txErrors++;
            Log("TX drain (%{public}s): DequeuePackets returned %u with a NULL packet -- "
                "stopping this drain (errors %llu)", why, count, ivars->txErrors);
            break;
        }

        ivars->txDequeued++;
        taken++;

        kern_return_t sret = txSubmitPacket(ivars, packet);
        if (sret == kIOReturnSuccess) {
            /* The packet now belongs to the in-flight transfer; TxComplete returns it. This
             * is the ONE path on which this function does not complete a packet it took. */
            sent++;
            continue;                           /* txInFlight is set, so the loop ends here */
        }
        failed++;
        txReturnPacket(ivars, packet, "not transmitted");
    }

    if (ivars->txInFlight && sent == 0) {
        /* Entered or left with someone else's transfer still running, so whatever is queued
         * stays queued. TxComplete will call this again, which is what releases it. */
        ivars->txDeferred++;
        Diag6("TX drain (%{public}s): a transfer is already in flight -- consumed nothing, "
              "leaving the queue alone (deferred %llu)", why, ivars->txDeferred);
        return;
    }

    Diag6("TX drain (%{public}s): took %u, submitted %u, refused %u; inFlight=%{public}s "
          "totals dequeued=%llu submitted=%llu returned=%llu frames=%llu rejected=%llu "
          "lost=%llu", why, taken, sent, failed, ivars->txInFlight ? "true" : "false",
          ivars->txDequeued, ivars->txSubmitted, ivars->txReturned, ivars->txFrames,
          ivars->txRejected, ivars->txLost);
}

/* The TX doorbell. Called when the stack makes the TX submission queue's data queue
 * non-empty; the packets themselves come from DequeuePackets, not from the notification. */
void
IMPL(SMSC95xxDriver, TxDataAvailable)
{
    (void)action;

    ivars->txDoorbells++;

    /* Tests the spec's "everything serialises on one dispatch queue" assumption. It should
     * now hold by construction -- this handler runs on the queue set for the target method
     * of its OSAction, the same queue as the pipe completions -- so if this ever fires the
     * assumption is wrong somewhere it was believed safe: stop and reconsider rather than
     * adding a lock and moving on. */
    if (ivars->inTxDataAvailable) {
        Log("*** TxDataAvailable RE-ENTERED: callbacks are NOT serialised ***");
    }
    ivars->inTxDataAvailable = true;

    /* The other half of the same assumption, and checkable rather than asserted: this handler
     * is meant to run on the driver's Default queue, which is the queue the four packet
     * queues were built with. OnQueue() is inclusive -- it can also return true for a queue
     * that invoked ours -- so a quiet run is weak evidence, but a false here is proof the
     * datapath is split across two queues and the no-locking assumption is void. */
    if (ivars->dispatchQueue != nullptr && !ivars->dispatchQueue->OnQueue()) {
        Log("*** TxDataAvailable NOT on the driver's dispatch queue ***");
    }

    /* A notification dispatched just before teardown can still arrive after the queues are
     * gone. Nothing has been dequeued at this point, so there is nothing to account for. */
    if (ivars->txSubmit == nullptr || ivars->txComplete == nullptr) {
        Log("TxDataAvailable after teardown: queues released, ignoring");
        ivars->inTxDataAvailable = false;
        return;
    }

    Diag6("TxDataAvailable #%llu: doorbell (inFlight=%{public}s dequeued=%llu returned=%llu "
          "frames=%llu errors=%llu)", ivars->txDoorbells,
          ivars->txInFlight ? "true" : "false", ivars->txDequeued, ivars->txReturned,
          ivars->txFrames, ivars->txErrors);

    txDrainSubmission(ivars, "doorbell");

    ivars->inTxDataAvailable = false;
}

/* Hand ONE received frame to the network stack, and account for the packet either way.
 *
 * ONE packet per call, not a batch, and that is a correctness requirement rather than a
 * simplification. enqueuePackets() in DriverKit 25.5 is a sixteen-instruction wrapper:
 *
 *     ldr x16,[x0] ... ldr x8,[x16,#0x58]! ; blraa  -> virtual EnqueuePackets(packets, count)
 *     cmp  w0, #0x0
 *     mov  w8, #0x2e8 ; movk w8, #0xe000, lsl #16   ; kIOReturnOverrun
 *     csel w0, w8, wzr, eq
 *
 * So its IOReturn carries exactly one bit of information: whether the count EnqueuePackets
 * accepted was zero. A batch of sixteen of which one is accepted returns kIOReturnSuccess and
 * gives no way to learn that the other fifteen were refused -- and EnqueuePackets breaks out
 * of its loop on the first packet it cannot take, so a partial accept is the normal shape of
 * failure, not a corner case. Those refused packets would be neither enqueued nor deallocated,
 * i.e. leaked, and a pool that quietly shrinks looks like a stall minutes later rather than
 * like the enqueue failure it is. With count == 1 the return value is unambiguous.
 *
 * The cost is one call per frame on a 10 Mb/s half-duplex segment, which is nothing.
 *
 * Returns true if the stack took the packet. On failure the packet is returned to the pool,
 * because the alternative is to lose it.
 *
 * `fromSubmitQueue` only shapes the log: a packet that came off the RX submission queue and is
 * being handed back to the pool is a last-resort disposal of a buffer the kernel considers
 * part of its RX ring, and that is worth being able to see in the trace. */
static bool
deliverPacket(IOUserNetworkRxCompletionQueue *queue, IOUserNetworkPacketBufferPool *pool,
              IOUserNetworkPacket *packet, bool fromSubmitQueue)
{
    IOReturn ret = queue->enqueuePackets(&packet, 1);
    if (ret == kIOReturnSuccess) {
        return true;
    }

    Log("RxComplete: enqueuePackets(1) FAILED: 0x%x%{public}s -- the stack accepted ZERO "
        "packets (that is all this status means; it is not a capacity report). Returning the "
        "packet, which came from %{public}s, to the pool",
        ret, (ret == kIOReturnOverrun) ? " (kIOReturnOverrun)" : "",
        fromSubmitQueue ? "the RX submission queue" : "the pool");

    IOReturn dret = pool->deallocatePacket(packet);
    if (dret != kIOReturnSuccess) {
        Log("RxComplete: deallocatePacket after the failed enqueue also failed: 0x%x -- 1 "
            "packet is LOST to the pool", dret);
    }
    return false;
}

/* Obtain one empty packet to receive a frame into.
 *
 * THE RX SUBMISSION QUEUE IS THE SOURCE, and getting that wrong is why receive used to fail
 * with kIOReturnOverrun on the very first frame.
 *
 * The family's model is symmetric with transmit, which works: the stack enqueues packets on
 * the TX SUBMISSION queue, the driver dequeues them, transmits, and returns them on the TX
 * COMPLETION queue. Receive is the mirror -- the stack supplies empty buffers on the RX
 * SUBMISSION queue, the driver dequeues those, fills them, and returns them on the RX
 * COMPLETION queue. This driver used to allocate straight from the pool instead and never
 * touch rxSubmit at all, so it was handing rxComplete packets the kernel had never given it.
 *
 * The mechanism, read out of NetworkingDriverKit: EnqueuePackets calls
 * packet->_CompleteWithQueue(queue, direction) on each packet -- a kernel RPC -- and stops at
 * the first one that fails. A packet is "completed with" a queue it was submitted on, so a
 * bare pool allocation has nothing to complete against.
 *
 * The pool fallback is kept deliberately, not as belt and braces but as the experiment: if the
 * stack does not put anything on rxSubmit, the log says so explicitly and the enqueue then
 * fails exactly as before, which is itself the answer. The remaining lead in that case is
 * IOUserNetworkPacketPoller (NetworkingDriverKit/IOUserNetworkPacketPoller.iig), whose
 * PollAction is documented as checking "for packets that can be dequeued or enqueued". */
static IOUserNetworkPacket *
rxAcquirePacket(SMSC95xxDriver_IVars *ivars, bool *fromSubmitQueue)
{
    *fromSubmitQueue = false;

    if (ivars->rxSubmit != nullptr) {
        IOUserNetworkPacket *packet = nullptr;
        uint32_t             count  = ivars->rxSubmit->DequeuePackets(&packet, 1);

        if (count == 1 && packet != nullptr) {
            ivars->rxSubmitDequeued++;
            *fromSubmitQueue = true;
            if (ivars->rxSubmitDequeued <= SMSC95XX_RX_TRACE_PACKETS) {
                Diag7("RxComplete: took packet %llu from the RX submission queue",
                      ivars->rxSubmitDequeued);
            }
            return packet;
        }

        /* Loud, but only for the first few: if this is the steady state then receive cannot
         * work at all and the reason must be in the log, whereas an occasional empty queue
         * under load is just backpressure and must not flood. */
        ivars->rxSubmitEmpty++;
        if (ivars->rxSubmitEmpty <= SMSC95XX_RX_TRACE_PACKETS) {
            Log("RxComplete: the RX submission queue gave nothing (DequeuePackets -> %u, "
                "packet=%{public}s, empty #%llu). Falling back to allocating from the pool, "
                "which is what used to fail at enqueue -- if this repeats, the stack is not "
                "supplying RX buffers on this queue and the poller model is the next lead",
                count, packet != nullptr ? "set" : "null", ivars->rxSubmitEmpty);
        }
    } else {
        /* Rate-limited for the same reason as the branch above: this is called once per
         * received frame, and os_log throttles hard enough under a burst that a flood here
         * would take the rest of the attach's log with it. */
        ivars->rxSubmitEmpty++;
        if (ivars->rxSubmitEmpty <= SMSC95XX_RX_TRACE_PACKETS) {
            Log("RxComplete: there is no RX submission queue (rxSubmit is null, empty #%llu) "
                "-- falling back to the pool", ivars->rxSubmitEmpty);
        }
    }

    IOUserNetworkPacket *packet = nullptr;
    IOReturn             pret   = ivars->pool->allocatePacket(&packet, kIOUserNetworkNonBlocking);
    if (pret != kIOReturnSuccess || packet == nullptr) {
        ivars->rxPoolFailures++;
        Log("RxComplete: allocatePacket also FAILED: 0x%x (packet=%{public}s, pool failures "
            "%llu)", pret, packet != nullptr ? "set" : "null", ivars->rxPoolFailures);
        return nullptr;
    }
    return packet;
}

/* Decode one bulk IN transfer and deliver its frames, then re-arm.
 *
 * The parameter names come from the iig-generated _Args macro, so an unused one cannot be
 * marked __unused at the declaration -- discard it in the body instead. */
void
IMPL(SMSC95xxDriver, RxComplete)
{
    (void)action;

    /* First, before any branch can return: the transfer this completion belongs to is over,
     * so the buffer is free for the next one. Clearing it here rather than at each exit is
     * what makes armRxRead's "already outstanding" check mean the hardware state and not
     * merely "we have been here before". */
    ivars->rxArmed = false;
    ivars->rxCompletions++;

    /* The RX half of the serialisation check TxDataAvailable already carries, and the pair
     * the code comment in this file asked Task 7 to add. Both are diagnostics, not locks: if
     * either fires, the "one dispatch queue, therefore no locking" design is wrong and has to
     * be revisited rather than patched. */
    if (ivars->inRxComplete) {
        Log("*** RxComplete RE-ENTERED: callbacks are NOT serialised ***");
    }
    ivars->inRxComplete = true;
    if (ivars->dispatchQueue != nullptr && !ivars->dispatchQueue->OnQueue()) {
        Log("*** RxComplete NOT on the driver's dispatch queue ***");
    }

    Diag7("RxComplete #%llu: status 0x%x, %u bytes, ts %llu (rxRunning=%{public}s, "
          "frames=%llu dropped=%llu enqueued=%llu)", ivars->rxCompletions, status,
          actualByteCount, completionTimestamp, ivars->rxRunning ? "true" : "false",
          ivars->rxFrames, ivars->rxDropped, ivars->rxEnqueued);

    /* EXIT 1: aborted. Normal at teardown -- teardownDatapath aborts the pipe on purpose --
     * and never fatal. Nothing has been allocated yet. Deliberately does NOT re-arm: the
     * buffer this would read into is about to be released. */
    if (status == kIOReturnAborted) {
        Log("RxComplete: aborted, not re-arming");
        ivars->inRxComplete = false;
        return;
    }

    /* EXIT 2: the interface has been disabled. An outstanding read survives
     * setInterfaceEnable(false), so one more completion can land afterwards; drop whatever it
     * carried rather than enqueue to a queue the stack has stopped draining, and let the loop
     * wind down by not re-arming. */
    if (!ivars->rxRunning) {
        Log("RxComplete: RX is not running (interface disabled or tearing down): "
            "discarding %u bytes and not re-arming", actualByteCount);
        ivars->inRxComplete = false;
        return;
    }

    /* EXIT 3: teardown has released what decoding needs. rxRunning should already have
     * caught this, so reaching here means teardown ordering changed; say so rather than
     * dereference a released object. Nothing allocated, and no re-arm. */
    if (ivars->rxBytes == nullptr || ivars->pool == nullptr || ivars->rxComplete == nullptr) {
        Log("RxComplete: datapath released under us (rxBytes=%{public}s pool=%{public}s "
            "rxComplete=%{public}s) -- discarding %u bytes and not re-arming",
            ivars->rxBytes    != nullptr ? "set" : "null",
            ivars->pool       != nullptr ? "set" : "null",
            ivars->rxComplete != nullptr ? "set" : "null", actualByteCount);
        ivars->inRxComplete = false;
        return;
    }

    /* EXIT 4: a failed transfer. Nothing allocated. Re-arm, because a single failure is not a
     * reason to stop receiving -- but clear a stall first, since every subsequent transfer on
     * a stalled pipe would fail the same way.
     *
     * Through rearmRxRead with carriedData=false: a failed transfer delivered nothing, and a
     * pipe that fails instantly has exactly the same busy-loop shape as a device with nothing
     * to give, so it belongs behind the same guard. */
    if (status != kIOReturnSuccess) {
        ivars->rxCompletionErrors++;
        Log("RxComplete: status 0x%x after %u bytes (error #%llu)", status, actualByteCount,
            ivars->rxCompletionErrors);
        if (status == kUSBHostReturnPipeStalled) {
            kern_return_t cret = ivars->pipeIn->ClearStall(true);
            Log("RxComplete: bulk IN was STALLED; ClearStall(true) -> 0x%x", cret);
        }
        rearmRxRead(false);
        ivars->inRxComplete = false;
        return;
    }

    ivars->rxByteCount += actualByteCount;

    /* EXIT 5: a successful transfer of nothing. Not an error and not evidence of a fault --
     * it is what a link with no traffic looks like if the host controller ever completes a
     * zero-length read at all.
     *
     * This is the path that produced the busy loop: re-arming unconditionally here, against a
     * device that completes the next read instantly, is a spin with no I/O wait in it. It now
     * goes through rearmRxRead, which re-arms immediately for the first few and then hands
     * the job to the backoff timer. */
    if (actualByteCount == 0) {
        ivars->rxZeroLength++;
        Diag7("RxComplete: zero-length completion (#%llu) -- idle link",
              ivars->rxZeroLength);
        rearmRxRead(false);
        ivars->inRxComplete = false;
        return;
    }

    /* EXIT 6: the real path. Walk the records: one transfer carries several frames, each a
     * 4-byte status word followed by frame data, 4-byte aligned. smsc95xx_rx_next returns
     * false at the end of the buffer and also on a malformed record, which is what we want --
     * either way there is nothing more to decode. */
    {
        size_t          offset = 0;
        const uint8_t  *frame = nullptr;
        size_t          frameLen = 0;
        uint32_t        rxStatus = 0;

        uint32_t             records = 0;
        uint32_t             delivered = 0;
        uint32_t             dropped = 0;
        bool                 noBuffer = false;

        while (smsc95xx_rx_next(ivars->rxBytes, actualByteCount, &offset,
                                &frame, &frameLen, &rxStatus)) {
            records++;
            ivars->rxRecords++;

            /* The raw status word for EVERY record, decoded, whether or not it is dropped
             * below. Six of these bits have never been observed set on this hardware, so a
             * frame that trips one is worth having in the log verbatim. */
            Diag7("RxComplete: record %u: status 0x%08x len-field %u frame_len %zu "
                  "next-offset %zu | ERRSUM=%d FILTFAIL=%d BCAST=%d MCAST=%d FTYPE=%d "
                  "LENERR=%d RUNT=%d TOOLONG=%d COLL=%d WDOG=%d MIIERR=%d DRIB=%d CRCERR=%d",
                  records, rxStatus,
                  (rxStatus & SMSC95XX_RX_STS_LEN_MASK) >> SMSC95XX_RX_STS_LEN_SHIFT,
                  frameLen, offset,
                  (rxStatus & SMSC95XX_RX_STS_ERROR_SUM)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_FILTER_FAIL)  ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_BROADCAST)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_MULTICAST)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_FRAME_TYPE)   ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_LENGTH_ERROR) ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_RUNT)         ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_TOO_LONG)     ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_COLLISION)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_WATCHDOG)     ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_MII_ERROR)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_DRIBBLING)    ? 1 : 0,
                  (rxStatus & SMSC95XX_RX_STS_CRC_ERROR)    ? 1 : 0);

            if ((rxStatus & (SMSC95XX_RX_STS_ERROR_SUM | SMSC95XX_RX_STS_FILTER_FAIL)) != 0) {
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: DROP record %u: status 0x%08x has%{public}s%{public}s "
                    "(total dropped %llu)", records, rxStatus,
                    (rxStatus & SMSC95XX_RX_STS_ERROR_SUM)   ? " ERROR_SUM"   : "",
                    (rxStatus & SMSC95XX_RX_STS_FILTER_FAIL) ? " FILTER_FAIL" : "",
                    ivars->rxDropped);
                continue;
            }
            /* frame_len INCLUDES the trailing 4-byte Ethernet CRC the hardware leaves in
             * place. The stack does not want it, and a record too short to hold one is not a
             * frame at all. */
            if (frameLen <= SMSC95XX_RX_CRC_LEN) {
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: DROP record %u: frame_len %zu is not longer than the %u-byte "
                    "CRC, status 0x%08x (total dropped %llu)", records, frameLen,
                    (unsigned int)SMSC95XX_RX_CRC_LEN, rxStatus, ivars->rxDropped);
                continue;
            }
            size_t payloadLen = frameLen - SMSC95XX_RX_CRC_LEN;
            /* Memory safety, not policy: smsc95xx_rx_next only checks that the record fits
             * the 4096-byte transfer buffer, and the pool's packet buffers are 2048 bytes. */
            if (payloadLen > SMSC95XX_RX_MAX_PAYLOAD) {
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: DROP record %u: payload %zu exceeds the %u-byte maximum this "
                    "driver will copy, status 0x%08x (total dropped %llu)", records,
                    payloadLen, (unsigned int)SMSC95XX_RX_MAX_PAYLOAD, rxStatus,
                    ivars->rxDropped);
                continue;
            }

            /* A buffer to receive into: the RX submission queue first, the pool as the
             * labelled fallback. See rxAcquirePacket. */
            bool                 fromSubmitQueue = false;
            IOUserNetworkPacket *packet = rxAcquirePacket(ivars, &fromSubmitQueue);
            if (packet == nullptr) {
                /* No buffer from either source: drop the rest of the transfer and rely on
                 * stack backpressure rather than blocking the completion handler. */
                ivars->rxDropped++;
                dropped++;
                noBuffer = true;
                Log("RxComplete: no packet available from the submission queue or the pool at "
                    "record %u -- abandoning the rest of this transfer (total dropped %llu)",
                    records, ivars->rxDropped);
                break;
            }

            /* SAFETY NET, deliberately kept even though the pool is now created with
             * PoolFlagMapToDext and this address should always be valid.
             *
             * When the pool was created without that flag, getDataVirtualAddress()
             * returned pool-relative OFFSETS -- 0xa800, 0xc000, 0xd800 were packets 21,
             * 24 and 27 at 2048 bytes each -- and the memcpy below faulted inside
             * _platform_memmove. That alone would be a driver crash, but IOKit relaunches
             * a dead dext, the driver died on every received frame, and the respawn loop
             * exhausted SPTM shared regions and PANICKED THE KERNEL.
             *
             * So the cost of a bad address here is not a crash, it is the user's machine.
             * A one-comparison check that turns it into a dropped frame and a log line is
             * worth keeping permanently. Anything below 1 MB cannot be a mapped buffer in
             * this address space, while it comfortably covers every offset this pool could
             * produce (64 x 2048 = 128 KB).
             *
             * getDataOff() is added because getDataVirtualAddress() returns the buffer BASE and
             * does NOT include the packet's data offset -- it is a bare ivar load in the family,
             * and getDataOff() reads a different ivar. This used to write at the base while the
             * stack read from base + offset, which put every received frame 8 bytes early. Same
             * root cause as the transmit-side misalignment; see txSubmitPacket. */
            size_t   dataOff = packet->getDataOff();
            uint64_t base    = packet->getDataVirtualAddress();
            uint64_t dst     = base + dataOff;
            if (base < 0x100000ull) {
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: REFUSING to copy: getDataVirtualAddress() returned "
                    "0x%llx, which is not a mapped address -- this is the pool-not-mapped "
                    "bug (PoolFlagMapToDext missing) and copying would fault and, via the "
                    "dext respawn loop, panic the kernel. Dropping record %u "
                    "(total dropped %llu)", base, records, ivars->rxDropped);
                ivars->pool->deallocatePacket(packet);
                break;
            }
            /* The offset eats into the 2048-byte buffer, so the bound has to account for it
             * rather than assume SMSC95XX_RX_MAX_PAYLOAD alone is safe. */
            if (dataOff + payloadLen > SMSC95XX_POOL_BUFFER_SIZE) {
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: DROP record %u: data offset %zu plus payload %zu exceeds the "
                    "%u-byte packet buffer (total dropped %llu)", records, dataOff, payloadLen,
                    (unsigned int)SMSC95XX_POOL_BUFFER_SIZE, ivars->rxDropped);
                ivars->pool->deallocatePacket(packet);
                continue;
            }

            memcpy(reinterpret_cast<void *>(dst), frame, payloadLen);
            IOReturn sret = packet->setDataLength((uint32_t)payloadLen);
            if (sret != kIOReturnSuccess) {
                /* Length is what tells the stack how much of the buffer is a frame, so a
                 * packet whose length did not take cannot be delivered. Return it. */
                ivars->rxDropped++;
                dropped++;
                Log("RxComplete: setDataLength(%zu) failed: 0x%x at record %u -- returning "
                    "the packet to the pool (total dropped %llu)", payloadLen, sret, records,
                    ivars->rxDropped);
                IOReturn dret = ivars->pool->deallocatePacket(packet);
                if (dret != kIOReturnSuccess) {
                    ivars->rxLost++;
                    Log("RxComplete: deallocatePacket also failed: 0x%x -- 1 packet LOST "
                        "(total lost %llu)", dret, ivars->rxLost);
                }
                continue;
            }

            Diag7("RxComplete: record %u accepted: %zu payload bytes copied "
                  "(frame_len %zu less %u CRC) to 0x%llx = base 0x%llx + offset %zu",
                  records, payloadLen, frameLen, (unsigned int)SMSC95XX_RX_CRC_LEN,
                  dst, base, dataOff);

            ivars->rxFrames++;

            /* Delivered immediately rather than batched: see deliverPacket. The packet is
             * either the stack's now or back in the pool; either way it is accounted for
             * before the next record is decoded, so no exit from this loop can leak it. */
            if (deliverPacket(ivars->rxComplete, ivars->pool, packet, fromSubmitQueue)) {
                delivered++;
                ivars->rxEnqueued++;
            } else {
                ivars->rxEnqueueFailures++;
                ivars->rxDropped++;
                dropped++;
            }
        }

        /* "walk ended at offset N of M", not "N of M consumed": the offset is advanced by the
         * 4-byte-ALIGNED record size, so after the last record it can legitimately sit past
         * actualByteCount -- a 106-byte transfer of one 102-byte record ends at 108. The loop
         * then terminates on the next bounds check, which is correct. Reporting that as
         * "108 of 106 bytes consumed" read like a buffer overrun and was purely a wording bug.
         * An offset well SHORT of actualByteCount is the interesting case: it means the walk
         * stopped early on a malformed record. */
        Diag7("RxComplete #%llu: %u bytes -> %u records, %u delivered, %u dropped, walk ended "
              "at offset %zu of %u%{public}s", ivars->rxCompletions, actualByteCount,
              records, delivered, dropped, offset, actualByteCount,
              noBuffer ? "  <-- NO RECEIVE BUFFER AVAILABLE, remainder of the transfer discarded"
                            : "");
        if (records == 0) {
            Log("RxComplete: %u bytes decoded to ZERO records -- the first status word is "
                "malformed or the transfer is not framed as expected", actualByteCount);
        }
    }

    /* This transfer carried bytes, so the device is not idle: re-arm immediately and reset
     * any backoff. Reaching here having decoded zero records still counts as data -- the
     * transfer was not empty, the framing was wrong, and that is a different fault from an
     * idle link. */
    rearmRxRead(true);
    ivars->inRxComplete = false;
}

/* One bulk OUT transfer has finished. Return its packet, recover the pipe if it stalled, and
 * pull whatever arrived while it was running.
 *
 * EVERY path through this function returns the packet exactly once -- success, failure, stall
 * and abort all fall through to the same single txReturnPacket call. That is on purpose:
 * a status-specific return would be one `if` away from a packet that is never handed back,
 * and the symptom of that (transmit stopping dead after 64 frames, with nothing logged) is
 * expensive to diagnose from the outside. */
void
IMPL(SMSC95xxDriver, TxComplete)
{
    (void)action;

    /* FIRST, before any branch or log line: this transfer is over, so the scratch buffer is
     * free and the packet is ours to dispose of. Taking the pointer and clearing the ivar in
     * the same breath is what makes double completion impossible -- a second completion for
     * the same transfer, however it arose, would find a null packet and return nothing. */
    IOUserNetworkPacket *packet = ivars->txPacket;
    ivars->txPacket   = nullptr;
    ivars->txInFlight = false;
    ivars->txCompletions++;

    /* The TX half of the serialisation diagnostic the other two callbacks carry. */
    if (ivars->inTxComplete) {
        Log("*** TxComplete RE-ENTERED: callbacks are NOT serialised ***");
    }
    ivars->inTxComplete = true;
    /* Skipped once teardown has started, and this is not laziness: teardownDatapath calls
     * Abort(kIOUSBAbortSynchronous), whose completion the SDK says may be delivered while the
     * abort is still on the stack -- i.e. on whichever thread is running Stop(), which is not
     * the driver's dispatch queue. Checking it there would report a serialisation failure
     * every single teardown and teach the reader to ignore the line. */
    if (ivars->txDatapathReady && ivars->dispatchQueue != nullptr
            && !ivars->dispatchQueue->OnQueue()) {
        Log("*** TxComplete NOT on the driver's dispatch queue ***");
    }

    Diag6("TxComplete #%llu: status 0x%x, %u bytes, ts %llu (packet %{public}s, "
          "datapath %{public}s, submitted=%llu frames=%llu errors=%llu)",
          ivars->txCompletions, status, actualByteCount, completionTimestamp,
          packet != nullptr ? "held" : "NONE",
          ivars->txDatapathReady ? "live" : "tearing down", ivars->txSubmitted,
          ivars->txFrames, ivars->txErrors);

    if (packet == nullptr) {
        /* A completion with no packet recorded. Either a transfer completed twice or one was
         * submitted without recording its packet -- both are bookkeeping bugs, and both would
         * otherwise be invisible, so this is the line that catches them. */
        Log("TxComplete: NO packet was recorded for this transfer (status 0x%x, %u bytes) -- "
            "either a completion was delivered twice or a submit did not record its packet; "
            "the one-in-flight bookkeeping is wrong", status, actualByteCount);
    }

    const char *why = "transmitted";

    if (status == kIOReturnSuccess) {
        ivars->txFrames++;
        ivars->txBytesSent += actualByteCount;
        Diag6("TxComplete: transfer #%llu SENT %u bytes (frame %llu, %llu bytes total on the "
              "wire including %u-byte headers)", ivars->txCompletions, actualByteCount,
              ivars->txFrames, ivars->txBytesSent, (unsigned int)SMSC95XX_TX_HEADER_LEN);
    } else if (status == kIOReturnAborted) {
        /* Normal at teardown -- teardownDatapath aborts the pipe on purpose -- and never
         * fatal. Counted separately from real errors so an error total stays meaningful. */
        ivars->txAborted++;
        why = "transfer aborted";
        Log("TxComplete: aborted after %u bytes (teardown, abort #%llu) -- the packet is still "
            "returned to the stack, and nothing further is consumed", actualByteCount,
            ivars->txAborted);
    } else {
        ivars->txErrors++;
        why = "transfer failed";
        Log("TxComplete: status 0x%x after %u bytes (error #%llu)", status, actualByteCount,
            ivars->txErrors);
        /* A stalled bulk endpoint stays halted until it is cleared, so every subsequent
         * transfer would fail the same way: an unrecovered stall kills transmit for the whole
         * life of the attach. withRequest=true, as the SDK header recommends, so the device's
         * data toggle is resynchronised with the host's rather than left to drift. */
        if (status == kUSBHostReturnPipeStalled) {
            if (ivars->pipeOut != nullptr) {
                kern_return_t cret = ivars->pipeOut->ClearStall(true);
                ivars->txStalls++;
                Log("TxComplete: bulk OUT 0x%02x was STALLED; ClearStall(true) -> 0x%x "
                    "(stall #%llu)", SMSC95XX_EP_BULK_OUT, cret, ivars->txStalls);
            } else {
                Log("TxComplete: bulk OUT was STALLED but the pipe is already released -- "
                    "cannot clear it");
            }
        }
    }

    /* THE ONE RETURN, on every path. */
    txReturnPacket(ivars, packet, why);

    /* And then pull whatever arrived while that transfer was running. The doorbell is
     * edge-triggered, so its notification for those packets has already been spent: without
     * this they would wait for the next unrelated enqueue, which looks like a stall. Gated
     * internally on txDatapathReady, so the aborted completion at teardown stops here. */
    txDrainSubmission(ivars, "completion");

    ivars->inTxComplete = false;
}
