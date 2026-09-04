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

/* THE PER-FRAME TRACES, compiled in only with `make TRACE=1`. These fire once per frame,
 * and os_log throttles hard enough under a burst to lose an entire attach's log, so they
 * must stay out of a normal build; what remains there is every error, refusal and guard
 * plus logDatapathCounters()'s totals. Disabled, they compile to `if (0) os_log(...)` so
 * format strings and arguments stay type-checked (see the DIAG5 macro in
 * SMSC95xxDriver.cpp). No trace argument may have a side effect.
 *
 *   DIAG7 -- receive half:  log stream --predicate 'eventMessage CONTAINS "DIAG7:"'
 *   DIAG6 -- transmit half: log stream --predicate 'eventMessage CONTAINS "DIAG6:"'
 *   everything this driver logs: eventMessage CONTAINS "SMSC95xx"
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
/* Largest frame the transmit path will copy into the TX buffer, as a memory-safety
 * bound: the memcpy below is only as safe as the length driving it, and 1518 (a
 * VLAN-tagged maximum-length frame) is the largest thing that could legitimately arrive.
 * Anything larger is refused. smsc95xx_tx_prepend then applies the stricter protocol
 * limit SMSC95XX_FRAME_MAX (1514), so 1515..1518 is refused too, with a different log
 * line -- "implausible length" and "the protocol says no" are different faults. */
#define SMSC95XX_TX_MAX_FRAME 1518

/* Worst case one frame plus its 8-byte TX header, and the exact bound smsc95xx_tx_prepend is
 * given so a short buffer could never be written past. SMSC95XX_TX_MAX_FRAME rather than
 * SMSC95XX_FRAME_MAX (1514) leaves the four bytes of slack a CRC would occupy; the chip
 * appends the CRC itself, so those bytes are headroom, not payload. */
#define SMSC95XX_TX_BUFFER_LEN (SMSC95XX_TX_HEADER_LEN + SMSC95XX_TX_MAX_FRAME)

/* How many transmitted packets get the full framing trace, including the data offset.
 * Whether the reserved headroom is present is a fixed property of the family and pool, so
 * a handful of lines at the start of each attach answers it. */
#define SMSC95XX_TX_TRACE_PACKETS 8

/* How many received packets get the full acquisition trace: whether the RX submission
 * queue supplies buffers is a fixed property of the family, so a handful of lines at the
 * start of an attach answers it and nothing after that is needed. (Frames are handed to
 * the stack one at a time, not batched -- see deliverPacket for why.) */
#define SMSC95XX_RX_TRACE_PACKETS 8

/* Largest RX payload (frame without its CRC) that will be copied into a packet, as a
 * memory-safety bound: smsc95xx_rx_next only guarantees a record fits the transfer
 * buffer, so a corrupt status word can yield a length near SMSC95XX_RX_BUFFER_LEN, which
 * would overrun the 2048-byte packet buffer. 1518 is a VLAN-tagged maximum-length frame
 * -- larger than SMSC95XX_FRAME_MAX (1514) so a tagged frame is not dropped, and well
 * under 2048 so the copy always fits. */
#define SMSC95XX_RX_MAX_PAYLOAD 1518

/* SMSC95XX_POOL_BUFFER_SIZE (shared via the ivars header) bounds every copy in and out
 * of a packet. Neither direction can use a frame-length bound alone: the copy starts at
 * getDataOff() bytes into the buffer, so offset plus length is what has to fit. On TX
 * that bounds a READ past the end of a packet, which is exactly as fatal as a bad write.
 *
 * Per-frame fault paths log their first few occurrences in full and then fall silent;
 * the totals stay visible through logDatapathCounters. os_log throttles hard under a
 * sustained burst, and one line per damaged frame at wire rate can wipe the rest of the
 * attach's log from the store. */
#define SMSC95XX_RX_FAULT_LOG_LIMIT 8

/* ---- the idle-RX backoff -------------------------------------------------------------
 *
 * A completion that delivers nothing -- a zero-length success or a failed transfer --
 * can complete the next read instantly, so re-arming unconditionally spins a CPU core
 * with no I/O wait in the loop. This bounds that: after a run of empty completions,
 * re-arming is handed to a timer.
 *
 * SMSC95XX_RX_IDLE_RUN_LIMIT -- consecutive empty completions re-armed immediately before
 *   the timer takes over. 8 is above any legitimate transient (the completion after an
 *   abort, the first read after enable) and far below any measurable cost.
 * SMSC95XX_RX_BACKOFF_NS -- 100 ms, so at most 10 arms/second while idle. Bounds the
 *   extra latency on the first frame after an idle period to 100 ms, well inside ARP and
 *   TCP SYN retransmit timers. Fixed, not escalating: escalation would trade a negligible
 *   cost for a non-obvious worst-case latency.
 * SMSC95XX_RX_BACKOFF_LEEWAY_NS -- 50 ms of coalescing slack; an idle poll need not be
 *   punctual.
 *
 * A frame arriving during the backoff is not lost: the chip buffers it in its RX FIFO and
 * hands it over on the next transfer, up to one interval late, which resets the run to
 * immediate re-arming. The FIFO is finite, so a burst filling one 100 ms window could in
 * principle overflow it -- not observed, and unprovable without hardware -- but the
 * interface is never left permanently deaf. */
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
    /* A transfer length we chose is written into these buffers, so a short segment would
     * turn every transfer into an overrun: check rather than assume. */
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

    /* The TX doorbell. CopyDataQueue hands us the submission queue's shared-memory data
     * queue (Copy, so retained; releaseDatapath releases it), and SetDataAvailableHandler
     * asks to be called when the stack makes it non-empty. The handler runs on the queue
     * set for its OSAction's target method -- the same queue as RxComplete/TxComplete --
     * so the datapath keeps one serialisation context. */
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

    /* The idle-RX backoff timer, on the same dispatch queue as everything else so its
     * handler serialises with RxComplete and needs no locking. A failure fails setup:
     * without the timer the only alternatives are the hot loop it prevents and a receive
     * path that stops for good. */
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

    /* LAST, after every object the transmit path dereferences exists: this flag lets the
     * drain loop consume packets. Setting it earlier would let a doorbell hand a packet to
     * a pipe setup had not finished acquiring; quiesceDatapath clears it first. */
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

/* The permanent datapath totals: two lines, TX then RX. Unlike the DIAG6/DIAG7 tracing,
 * these survive a default build, so `log show` can answer "did the interface move
 * traffic, and did it lose anything" after the fact.
 *
 * The numbers to read first are the accounting identities, because a break in either is
 * a leak rather than a slowdown:
 *   TX  dequeued == returned + lost + (1 if a transfer is in flight)
 *   RX  records  == frames + dropped, and frames == enqueued
 * fromSubmitQ against submitQEmpty says which source the receive path is actually using.
 */
void
SMSC95xxDriver::logDatapathCounters(const char *why)
{
    /* Four short lines, not two long ones. A dext's os_log messages are clipped at a couple
     * of hundred characters, and the two combined lines exceeded that once the counters grew
     * to real values -- losing the tail fields, which is where the accounting identities
     * live. The identity each line proves is stated on the line itself so a reader does not
     * have to remember it. */
    Log("counters at %{public}s -- TX accounting: dequeued %llu = returned %llu + lost %llu "
        "+ inFlight %u  (identity must hold)",
        why, ivars->txDequeued, ivars->txReturned, ivars->txLost,
        ivars->txInFlight ? 1u : 0u);
    Log("counters at %{public}s -- TX detail: doorbells %llu drains %llu deferred %llu "
        "submitted %llu completions %llu frames %llu bytes %llu rejected %llu submitFail "
        "%llu errors %llu stalls %llu aborted %llu",
        why, ivars->txDoorbells, ivars->txDrains, ivars->txDeferred, ivars->txSubmitted,
        ivars->txCompletions, ivars->txFrames, ivars->txBytesSent, ivars->txRejected,
        ivars->txSubmitFailures, ivars->txErrors, ivars->txStalls, ivars->txAborted);

    Log("counters at %{public}s -- RX accounting: records %llu = frames %llu + dropped %llu, "
        "and frames = enqueued %llu  (identities must hold)",
        why, ivars->rxRecords, ivars->rxFrames, ivars->rxDropped, ivars->rxEnqueued);
    Log("counters at %{public}s -- RX detail: completions %llu errors %llu zeroLen %llu "
        "zeroRec %llu arms %llu armFail %llu bytes %llu enqFail %llu lost %llu "
        "fromSubmitQ %llu submitQEmpty %llu poolFail %llu",
        why, ivars->rxCompletions, ivars->rxCompletionErrors, ivars->rxZeroLength,
        ivars->rxZeroRecords, ivars->rxArmCount, ivars->rxArmFailures, ivars->rxByteCount,
        ivars->rxEnqueueFailures, ivars->rxLost, ivars->rxSubmitDequeued,
        ivars->rxSubmitEmpty, ivars->rxPoolFailures);
}

/* True for an Abort result that just means "the device or its pipe has already gone, so
 * there was nothing left to abort" -- routine when the dongle is unplugged, and observed as
 * kIOReturnNotReady on a yank. Anything else is a real refusal and must stay loud. */
static bool
abortResultIsBenign(kern_return_t ar)
{
    return ar == kIOReturnSuccess
        || ar == kIOReturnNoDevice        /* 0x2c0 no such device        */
        || ar == kIOReturnOffline         /* 0x2d7 device offline        */
        || ar == kIOReturnNotReady        /* 0x2d8 pipe gone with device */
        || ar == kIOReturnNotAttached     /* 0x2d9 device not attached   */
        || ar == kIOReturnNotResponding   /* 0x2ed device not responding */
        || ar == kIOReturnAborted;        /* 0x2eb already aborted       */
}

/* PHASE 1 of teardown: stop new work and abort the USB pipes. Releases nothing.
 *
 * IOService::Stop is documented to require that a driver "stop all activity and put your
 * driver in a quiescent state", cancelling in-progress asynchronous tasks and waiting for
 * their cancellation handlers before calling super. This is the half that can be done
 * synchronously: clearing the two gate flags stops new work being started, and
 * Abort(kIOUSBAbortSynchronous) is documented not to return until the aborted I/O has
 * completed -- a join for the hardware I/O, though not for the completion callbacks
 * (see the Abort comment below).
 *
 * The dispatch sources (the backoff timer and the TX doorbell) cannot be joined here --
 * their handlers run on the same queue and only Cancel()'s handler is documented to fire
 * after in-flight callbacks finish. Stop() Cancels them and calls releaseDatapath() from
 * the final cancellation handler. */
void
SMSC95xxDriver::quiesceDatapath(void)
{
    /* Stop the receive loop BEFORE anything is aborted. The synchronous Abort below can
     * deliver the aborted transfer's completion while this function is still on the stack;
     * RxComplete is what would re-arm, and with rxRunning already false it will not.
     *
     * rxArmed is NOT cleared here: the aborted transfer's completion clears it, and
     * clearing it early would let a late armRxRead() double-arm. */
    logDatapathCounters("datapath teardown");
    Diag7("quiesceDatapath: entered. rxRunning=%{public}s rxArmed=%{public}s "
          "OnQueue(Default)=%{public}s",
          ivars->rxRunning ? "true" : "false", ivars->rxArmed ? "true" : "false",
          (ivars->dispatchQueue != nullptr && ivars->dispatchQueue->OnQueue()) ? "true" : "false");
    Diag7("quiesceDatapath: idle-RX backoff state: idleRun=%llu active=%{public}s "
          "episodes=%llu wakes=%llu", ivars->rxIdleRun,
          ivars->rxBackoffActive ? "true" : "false", ivars->rxBackoffEpisodes,
          ivars->rxBackoffWakes);
    ivars->rxRunning = false;

    /* Close the transmit path to new work, for the same reason. The synchronous bulk OUT
     * Abort can deliver TxComplete from inside this function; with this flag false that
     * completion returns its own packet (the TX completion queue is still alive -- it is
     * released in releaseDatapath's caller, after every cancellation has run) and then
     * stops, rather than framing more packets.
     *
     * txInFlight is NOT cleared here, like rxArmed: the aborted completion clears it, and
     * clearing it early would let a drain slip a second transfer past the one-in-flight
     * rule. */
    ivars->txDatapathReady = false;
    Diag6("quiesceDatapath: TX state: inFlight=%{public}s packet=%{public}s doorbells=%llu "
          "drains=%llu dequeued=%llu submitted=%llu completions=%llu frames=%llu "
          "returned=%llu lost=%llu bytes=%llu rejected=%llu submitFail=%llu stalls=%llu "
          "aborted=%llu deferred=%llu errors=%llu",
          ivars->txInFlight ? "true" : "false",
          ivars->txPacket != nullptr ? "set (its completion has not run yet)" : "null",
          ivars->txDoorbells, ivars->txDrains, ivars->txDequeued, ivars->txSubmitted,
          ivars->txCompletions, ivars->txFrames, ivars->txReturned, ivars->txLost,
          ivars->txBytesSent, ivars->txRejected, ivars->txSubmitFailures, ivars->txStalls,
          ivars->txAborted, ivars->txDeferred, ivars->txErrors);

    /* Abort both pipes so the hardware issues no further I/O. This stops new completions;
     * it does NOT join the ones already pending.
     *
     * forClient MUST be nullptr. The header is explicit: "If NULL, all requests will be
     * aborted. Only control endpoints can specify a non-NULL value" -- and these are bulk
     * endpoints.
     *
     * kIOUSBAbortSynchronous is documented not to return until the aborted I/O has
     * completed, but that does NOT make it a join here: Stop runs on the same serial queue
     * that delivers the completions, so an aborted transfer's completion cannot run until
     * Stop returns.
     *
     * THE PIPE COMPLETIONS CANNOT BE JOINED AT ALL IN THIS ARCHITECTURE: an aborted
     * RxComplete can arrive after releaseDatapath has released the buffers, and
     * OSAction::Cancel does not prevent it -- its "after any in-flight callbacks finish"
     * covers callbacks currently EXECUTING, not one still queued for delivery behind Stop
     * (observed on hardware; see reference/teardown-quiescence.txt).
     *
     * So what makes that late completion safe is NOT a join, it is the guards, and they are
     * therefore load-bearing rather than belt-and-braces: releaseDatapath nulls rxBytes and
     * txBytes BEFORE releasing the descriptors; RxComplete's aborted branch returns before
     * touching any buffer and its EXIT 3 re-checks rxBytes/pool/rxComplete for null;
     * TxComplete finds txPacket null and txComplete gone and accounts for it. Do not remove
     * any of those checks on the theory that teardown has already joined everything.
     * (The actions are still cancelled: that is the documented way to stop any FUTURE
     * invocation, and it costs nothing.)
     *
     * The result is CHECKED: a silently refused Abort would leave live I/O against buffers
     * about to be released. abortResultIsBenign() above separates "the device is already
     * gone, so there is nothing to abort" -- routine on an unplug -- from a real refusal
     * like kIOReturnBadArgument, which must stay loud. */
    if (ivars->pipeIn != nullptr) {
        kern_return_t ar = ivars->pipeIn->Abort(kIOUSBAbortSynchronous, kIOReturnAborted,
                                                nullptr);
        if (!abortResultIsBenign(ar)) {
            Log("quiesceDatapath: Abort(bulk IN, synchronous) REFUSED: 0x%x -- not a "
                "device-gone status, so this is a real error; receive I/O was not aborted",
                ar);
        }
        Diag7("quiesceDatapath: Abort(bulk IN, synchronous) -> 0x%x; rxArmed is now "
              "%{public}s", ar, ivars->rxArmed ? "true (completion not seen yet)" : "false");
    }
    if (ivars->pipeOut != nullptr) {
        kern_return_t ar = ivars->pipeOut->Abort(kIOUSBAbortSynchronous, kIOReturnAborted,
                                                 nullptr);
        if (!abortResultIsBenign(ar)) {
            Log("quiesceDatapath: Abort(bulk OUT, synchronous) REFUSED: 0x%x -- not a "
                "device-gone status, so this is a real error; transmit I/O was not aborted",
                ar);
        }
        Diag7("quiesceDatapath: Abort(bulk OUT, synchronous) -> 0x%x", ar);
    }
    Diag7("quiesceDatapath: complete; pipes aborted, no new work accepted");
}

/* PHASE 2 of teardown: release the datapath objects.
 *
 * MUST NOT be called until every dispatch-source cancellation handler has fired -- see
 * Stop(). By that point nothing can still be executing on the dispatch queue, so these
 * releases cannot pull an object out from under a running handler. */
void
SMSC95xxDriver::releaseDatapath(void)
{
    /* The sources have been cancelled by Stop() (or, on the Start() failure path, were
     * never armed: no read is posted and no packet queue is enabled until
     * setInterfaceEnable, and an enabled timer with no deadline never fires). Cancelling
     * is terminal -- "after cancellation, the source can only be freed" -- so dropping our
     * references is all that is left. */
    OSSafeReleaseNULL(ivars->rxBackoffAction);
    OSSafeReleaseNULL(ivars->rxBackoffTimer);
    ivars->rxBackoffActive = false;
    ivars->rxIdleRun       = 0;

    OSSafeReleaseNULL(ivars->txDataAvailableAction);
    OSSafeReleaseNULL(ivars->txDataQueue);

    /* Actions before pipes: an action is what a completion is delivered through, and
     * nothing can still be pending once quiesceDatapath's synchronous aborts returned. */
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

    /* After a synchronous abort every completion has run, so this should already be false
     * with no packet held. If not, the one-in-flight bookkeeping is broken: the packet
     * cannot safely be completed now (a not-yet-delivered completion might still arrive,
     * and completing twice hands the stack a packet we still point at), so it is dropped
     * and counted. The pool is reclaimed when it is released. */
    if (ivars->txPacket != nullptr || ivars->txInFlight) {
        ivars->txLost++;
        Log("releaseDatapath: a TX was STILL in flight after a synchronous abort "
            "(inFlight=%{public}s packet=%{public}s) -- the packet is dropped rather than "
            "risk completing it twice (total lost %llu). The one-in-flight bookkeeping is "
            "wrong if this line ever appears.",
            ivars->txInFlight ? "true" : "false",
            ivars->txPacket != nullptr ? "set" : "null", ivars->txLost);
    }
    ivars->txPacket   = nullptr;
    ivars->txInFlight = false;
    /* Only now, with the pipes released and the buffers gone, is it certain no transfer can
     * be outstanding, so rxArmed becomes just a cleared flag. */
    ivars->rxArmed = false;
    Diag7("releaseDatapath: complete; pipes, buffers, sources and actions released");
}

/* Submit one bulk IN transfer into ivars->rxBuffer.
 *
 * Idempotent: with a transfer already outstanding it returns success without submitting a
 * second one. There is one RX buffer, so two overlapping AsyncIOs would have the device
 * writing the same memory twice; the postcondition callers care about ("a read is
 * outstanding") already holds. */
kern_return_t
SMSC95xxDriver::armRxRead(void)
{
    if (ivars->pipeIn == nullptr || ivars->rxAction == nullptr
            || ivars->rxBuffer == nullptr) {
        /* Reached if something arms after the datapath has been released. Not an error worth
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
            /* Once per episode, not per completion, so an idle device does not flood
             * the log. */
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
 * THE ACCOUNTING RULE THE WHOLE TRANSMIT PATH IS BUILT AROUND: every packet DequeuePackets
 * yields is handed back to the stack EXACTLY ONCE, on every path -- transmitted, refused,
 * or aborted at teardown. A packet consumed and never completed is gone from the pool for
 * good, so 64 of them wedge transmit silently; a packet completed twice hands the stack a
 * packet this driver still points at.
 *
 * Two things keep the rule auditable: txReturnPacket is the ONLY place a packet goes back,
 * and the drain loop takes ONE packet at a time and disposes of it before taking another,
 * so no batch of packets is ever held where an early return could strand it. */

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
        /* Only reachable if the completion queue was released while a packet was held,
         * which teardown ordering prevents. The packet is reclaimed with the pool, but it
         * never reached the stack, so count it. */
        ivars->txLost++;
        Log("TX: cannot complete a packet (%{public}s): the TX completion queue is gone -- "
            "1 packet LOST (total lost %llu)", why, ivars->txLost);
        return;
    }

    IOReturn ret = ivars->txComplete->enqueuePackets(&packet, 1);
    if (ret != kIOReturnSuccess) {
        /* Out of the submission queue and unable to go back, so it is lost -- logged
         * loudly, because silent loss surfaces later as a pool that never refills. */
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

    /* Where the frame starts: getDataVirtualAddress() returns the buffer BASE and does NOT
     * add the packet's data offset, so the frame begins at base + getDataOff(). Adding
     * getDataOff() is correct whatever getTxDataOffset() returns.
     *
     * The frame is copied into a persistent scratch buffer rather than transmitted in
     * place: AsyncIO takes a descriptor and a length but no offset, so a zero-copy
     * transmit would need a per-frame sub-descriptor (a kernel round trip and an
     * allocation per frame) against a memcpy of at most 1518 bytes. That trade-off is a
     * throughput question for later; the copy is known-safe. */
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

    /* Length guard: the copy below is only as safe as this number, so a length outside
     * the plausible range is refused rather than memcpy'd. */
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

    /* Address sanity check before the copy, the mirror of the receive-side guard:
     * reading past a buffer faults on every frame, which IOKit's respawn loop escalates
     * to a kernel panic. Refuse rather than memcpy. */
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

    /* Set BEFORE AsyncIO: the completion cannot run until this returns (same dispatch
     * queue), but ordering the state first leaves no window where the flags disagree with
     * reality, and the failure branch below undoes them rather than the success path
     * remembering to set them. */
    ivars->txInFlight = true;
    ivars->txPacket   = packet;

    /* completionTimeoutMs 0: the USB stack completes every accepted transfer one way or
     * another (an error on device removal, kIOReturnAborted on Abort), so a timeout would
     * only guard against a stack bug at the cost of dropping frames to a slow device. */
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
 * ONE packet per DequeuePackets call, not a batch: with one transfer in flight a second
 * packet can only be held, and a held packet is what the accounting rule avoids. Taking
 * them one at a time keeps the only packet this function is responsible for in `packet`.
 * Backpressure is applied by not consuming -- the packets stay in the submission queue.
 *
 * requestDequeue() is not used: the queue was built with the dispatch-queue overload of
 * Create and has no DequeueAction, and this already runs on the driver's dispatch queue,
 * so calling the drain directly is both simpler and free of assumptions. */
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
    /* The teardown gate: quiesceDatapath clears this before aborting the bulk OUT pipe,
     * and the aborted transfer's completion can call this from inside that abort.
     * Consuming a packet here would frame it into a buffer about to be released. */
    if (!ivars->txDatapathReady) {
        Diag6("TX drain (%{public}s): the datapath is being torn down -- not consuming "
              "(anything still queued is the stack's, and is never taken from it)", why);
        return;
    }

    uint32_t taken = 0;
    uint32_t sent  = 0;
    uint32_t failed = 0;

    /* Bounded without a counter: each iteration either submits (setting txInFlight and
     * ending the loop) or disposes of one packet, and the pool holds a fixed number, so
     * the stack cannot present more without first getting some back. */
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

    /* Guards on the "everything serialises on one dispatch queue, so no locking"
     * assumption. It holds by construction -- this handler runs on the same queue as the
     * pipe completions -- so a fire here means the assumption is void and the design must
     * be revisited, not patched with a lock. OnQueue() is inclusive, so a quiet run is
     * weak evidence but a false is proof. */
    if (ivars->inTxDataAvailable) {
        Log("*** TxDataAvailable RE-ENTERED: callbacks are NOT serialised ***");
    }
    ivars->inTxDataAvailable = true;
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
 * ONE packet per call, not a batch: enqueuePackets' IOReturn only says whether the
 * accepted count was zero, and EnqueuePackets stops at the first packet it cannot take.
 * A partially accepted batch is therefore indistinguishable from a fully accepted one,
 * and the refused packets would be neither enqueued nor deallocated -- a leak that
 * surfaces minutes later as a pool that never refills. With count == 1 the return value
 * is unambiguous, and one call per frame costs nothing at 10 Mb/s.
 *
 * Returns true if the stack took the packet. On failure the packet goes back to the
 * pool, because the alternative is to lose it. `fromSubmitQueue` only shapes the log:
 * handing a packet that came off the RX submission queue back to the pool disposes of a
 * buffer the kernel considers part of its RX ring, which is worth seeing in the trace. */
static bool
deliverPacket(SMSC95xxDriver_IVars *ivars, IOUserNetworkPacket *packet,
              bool fromSubmitQueue)
{
    IOReturn ret = ivars->rxComplete->enqueuePackets(&packet, 1);
    if (ret == kIOReturnSuccess) {
        return true;
    }

    ivars->rxEnqueueFailures++;
    if (ivars->rxEnqueueFailures <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
        Log("RxComplete: enqueuePackets(1) FAILED: 0x%x%{public}s -- the stack accepted "
            "ZERO packets (that is all this status means; it is not a capacity report). "
            "Returning the packet, which came from %{public}s, to the pool (failure %llu)",
            ret, (ret == kIOReturnOverrun) ? " (kIOReturnOverrun)" : "",
            fromSubmitQueue ? "the RX submission queue" : "the pool",
            ivars->rxEnqueueFailures);
    }

    IOReturn dret = ivars->pool->deallocatePacket(packet);
    if (dret != kIOReturnSuccess) {
        ivars->rxLost++;
        Log("RxComplete: deallocatePacket after the failed enqueue also failed: 0x%x -- 1 "
            "packet LOST (total lost %llu)", dret, ivars->rxLost);
    }
    return false;
}

/* Obtain one empty packet to receive a frame into.
 *
 * The RX submission queue is the source, mirroring transmit: the stack supplies empty
 * buffers on the RX submission queue, the driver dequeues them, fills them, and returns
 * them on the RX completion queue. A packet must be enqueued on the completion queue it
 * was submitted from -- EnqueuePackets completes each packet against its submission queue
 * -- so a buffer allocated straight from the pool has nothing to complete against and the
 * enqueue fails with kIOReturnOverrun.
 *
 * The pool fallback exists only for the case where the stack supplies nothing on
 * rxSubmit; the enqueue then fails, which the log records explicitly. */
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

        /* Loud, but only for the first few: a steady state here means receive cannot work
         * and the reason must be in the log, while an occasional empty queue under load is
         * just backpressure and must not flood. */
        ivars->rxSubmitEmpty++;
        if (ivars->rxSubmitEmpty <= SMSC95XX_RX_TRACE_PACKETS) {
            Log("RxComplete: the RX submission queue gave nothing (DequeuePackets -> %u, "
                "packet=%{public}s, empty #%llu) -- falling back to the pool, whose enqueue "
                "will fail if the stack is not supplying RX buffers on this queue",
                count, packet != nullptr ? "set" : "null", ivars->rxSubmitEmpty);
        }
    } else {
        /* Rate-limited for the same reason: called once per frame, and a flood would take
         * the rest of the attach's log with it. */
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

    /* First, before any branch can return: this transfer is over, so the buffer is free
     * for the next one. Clearing rxArmed here rather than at each exit is what makes
     * armRxRead's "already outstanding" check reflect the hardware state. */
    ivars->rxArmed = false;
    ivars->rxCompletions++;

    /* The RX half of the serialisation guard TxDataAvailable carries: if either fires, the
     * "one dispatch queue, therefore no locking" design is void and must be revisited. */
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

    /* EXIT 1: aborted. Normal at teardown -- quiesceDatapath aborts the pipe on purpose --
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
     * Re-arming unconditionally here would spin: a device that completes the next read
     * instantly leaves no I/O wait in the loop. rearmRxRead re-arms immediately for the
     * first few and then hands the job to the backoff timer. */
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

            /* The raw status word for every record, decoded, dropped or not: several of
             * these bits have never been observed set on this hardware. */
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
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: DROP record %u: status 0x%08x has%{public}s%{public}s "
                        "(total dropped %llu)", records, rxStatus,
                        (rxStatus & SMSC95XX_RX_STS_ERROR_SUM)   ? " ERROR_SUM"   : "",
                        (rxStatus & SMSC95XX_RX_STS_FILTER_FAIL) ? " FILTER_FAIL" : "",
                        ivars->rxDropped);
                }
                continue;
            }
            /* frame_len INCLUDES the trailing 4-byte Ethernet CRC the hardware leaves in
             * place. The stack does not want it, and a record too short to hold one is not a
             * frame at all. */
            if (frameLen <= SMSC95XX_RX_CRC_LEN) {
                ivars->rxDropped++;
                dropped++;
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: DROP record %u: frame_len %zu is not longer than the "
                        "%u-byte CRC, status 0x%08x (total dropped %llu)", records, frameLen,
                        (unsigned int)SMSC95XX_RX_CRC_LEN, rxStatus, ivars->rxDropped);
                }
                continue;
            }
            size_t payloadLen = frameLen - SMSC95XX_RX_CRC_LEN;
            /* Memory safety: smsc95xx_rx_next only checks the record fits the transfer
             * buffer, which is larger than a packet buffer. */
            if (payloadLen > SMSC95XX_RX_MAX_PAYLOAD) {
                ivars->rxDropped++;
                dropped++;
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: DROP record %u: payload %zu exceeds the %u-byte maximum "
                        "this driver will copy, status 0x%08x (total dropped %llu)", records,
                        payloadLen, (unsigned int)SMSC95XX_RX_MAX_PAYLOAD, rxStatus,
                        ivars->rxDropped);
                }
                continue;
            }

            /* A buffer to receive into: the RX submission queue first, the pool as the
             * fallback. See rxAcquirePacket. */
            bool                 fromSubmitQueue = false;
            IOUserNetworkPacket *packet = rxAcquirePacket(ivars, &fromSubmitQueue);
            if (packet == nullptr) {
                /* No buffer from either source: drop the rest of the transfer and rely on
                 * stack backpressure rather than blocking the completion handler. */
                ivars->rxDropped++;
                dropped++;
                noBuffer = true;
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: no packet available from the submission queue or the "
                        "pool at record %u -- abandoning the rest of this transfer (total "
                        "dropped %llu)", records, ivars->rxDropped);
                }
                break;
            }

            /* Address sanity check before the copy. A fault here is not one crash: the
             * driver would die on every received frame, and IOKit's respawn loop turns a
             * repeating dext crash into a kernel panic. Anything below 1 MB cannot be a
             * mapped buffer in this address space (an unmapped pool hands back small
             * pool-relative offsets), so refuse and drop rather than memcpy.
             *
             * getDataOff() is added because getDataVirtualAddress() returns the buffer
             * BASE and does not include the packet's data offset; the stack reads the
             * frame from base + offset. Same arithmetic as the transmit side. */
            size_t   dataOff = packet->getDataOff();
            uint64_t base    = packet->getDataVirtualAddress();
            uint64_t dst     = base + dataOff;
            if (base < 0x100000ull) {
                ivars->rxDropped++;
                dropped++;
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: REFUSING to copy: getDataVirtualAddress() returned "
                        "0x%llx, which is not a mapped address (is the pool missing "
                        "PoolFlagMapToDext?) -- copying would fault. Dropping record %u "
                        "(total dropped %llu)", base, records, ivars->rxDropped);
                }
                ivars->pool->deallocatePacket(packet);
                break;
            }
            /* The offset eats into the packet buffer, so the bound has to account for it
             * rather than assume SMSC95XX_RX_MAX_PAYLOAD alone is safe. */
            if (dataOff + payloadLen > SMSC95XX_POOL_BUFFER_SIZE) {
                ivars->rxDropped++;
                dropped++;
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: DROP record %u: data offset %zu plus payload %zu "
                        "exceeds the %u-byte packet buffer (total dropped %llu)", records,
                        dataOff, payloadLen, (unsigned int)SMSC95XX_POOL_BUFFER_SIZE,
                        ivars->rxDropped);
                }
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
                if (ivars->rxDropped <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                    Log("RxComplete: setDataLength(%zu) failed: 0x%x at record %u -- "
                        "returning the packet to the pool (total dropped %llu)", payloadLen,
                        sret, records, ivars->rxDropped);
                }
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

            /* Delivered immediately rather than batched: see deliverPacket. The packet is
             * either the stack's now or back in the pool; either way it is accounted for
             * before the next record is decoded, so no exit from this loop can leak it.
             * rxFrames counts only delivered frames, so the accounting identities hold:
             * records == frames + dropped, and frames == enqueued. */
            if (deliverPacket(ivars, packet, fromSubmitQueue)) {
                delivered++;
                ivars->rxFrames++;
                ivars->rxEnqueued++;
            } else {
                ivars->rxDropped++;
                dropped++;
            }
        }

        /* "walk ended at offset N", not "N consumed": the offset advances by the 4-byte-
         * aligned record size, so after the last record it can sit slightly past
         * actualByteCount, and the loop terminates on the next bounds check. An offset well
         * SHORT of actualByteCount means the walk stopped early on a malformed record. */
        Diag7("RxComplete #%llu: %u bytes -> %u records, %u delivered, %u dropped, walk ended "
              "at offset %zu of %u%{public}s", ivars->rxCompletions, actualByteCount,
              records, delivered, dropped, offset, actualByteCount,
              noBuffer ? "  <-- NO RECEIVE BUFFER AVAILABLE, remainder of the transfer discarded"
                            : "");
        if (records == 0) {
            ivars->rxZeroRecords++;
            if (ivars->rxZeroRecords <= SMSC95XX_RX_FAULT_LOG_LIMIT) {
                Log("RxComplete: %u bytes decoded to ZERO records -- the first status word "
                    "is malformed or the transfer is not framed as expected (occurrence "
                    "%llu)", actualByteCount, ivars->rxZeroRecords);
            }
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

    /* FIRST, before any branch or log line: this transfer is over, so the scratch buffer
     * is free and the packet is ours to dispose of. Taking the pointer and clearing the
     * ivar together makes double completion impossible -- a second completion for the same
     * transfer finds a null packet and returns nothing. */
    IOUserNetworkPacket *packet = ivars->txPacket;
    ivars->txPacket   = nullptr;
    ivars->txInFlight = false;
    ivars->txCompletions++;

    /* The TX half of the serialisation guard the other two callbacks carry. Skipped once
     * teardown has started: the synchronous Abort delivers this completion on whichever
     * thread runs Stop(), not the dispatch queue, so the check would fire every teardown. */
    if (ivars->inTxComplete) {
        Log("*** TxComplete RE-ENTERED: callbacks are NOT serialised ***");
    }
    ivars->inTxComplete = true;
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
        /* Normal at teardown -- quiesceDatapath aborts the pipe on purpose -- and never
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
