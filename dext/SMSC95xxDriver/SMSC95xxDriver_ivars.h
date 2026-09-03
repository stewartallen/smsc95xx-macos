/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SMSC95xxDriver's instance variables.
 *
 * The class is built from two translation units -- SMSC95xxDriver.cpp owns the control
 * path (registers, MII, EEPROM, chip init, the network-stack plumbing) and
 * SMSC95xxDriver_datapath.cpp owns the bulk endpoints -- and both dereference `ivars`,
 * so the layout has to be one shared definition rather than a copy in each file. iig
 * only forward-declares SMSC95xxDriver_IVars in the generated header; defining it is
 * ours to do, exactly once.
 *
 * Pointer members are forward-declared rather than #included: nothing here calls a
 * method, and each .cpp already includes the real headers for the objects it touches.
 */

#ifndef SMSC95xxDriver_ivars_h
#define SMSC95xxDriver_ivars_h

#include <stdint.h>

#include "smsc95xx_regs.h"      /* SMSC95XX_MAC_LEN */

class IOUSBHostInterface;
class IOUSBHostPipe;
class IOBufferMemoryDescriptor;
class IODispatchQueue;
class IODataQueueDispatchSource;
class IOTimerDispatchSource;
class OSAction;
class IOUserNetworkPacket;
class IOUserNetworkPacketBufferPool;
class IOUserNetworkTxSubmissionQueue;
class IOUserNetworkRxSubmissionQueue;
class IOUserNetworkTxCompletionQueue;
class IOUserNetworkRxCompletionQueue;

/* `interface` is the matched provider and the ONLY USB object this class holds, so it is
 * BORROWED: close it if we opened it, never release it. There is deliberately no device
 * handle here. Vendor control transfers do reach the device, but IOUSBHostInterface is
 * the object that carries them: DeviceRequest with kIOUSBDeviceRequestRecipientDevice
 * issues the request to the device on our behalf (see readRegister/writeRegister), and
 * the datapath takes its pipes from the interface too. Selecting the configuration is
 * SMSC95xxUSBDevice's job. So do not re-add a CopyDevice() handle: nothing would read it,
 * and it could not be opened anyway -- IOUSBHostDevice admits one opener and the
 * device-level driver holds that session.
 * `ctrlBuffer` is a 4-byte IOBufferMemoryDescriptor used for all vendor control
 * transfers (reads and writes). `ctrlBytes` is the mapped address of ctrlBuffer. */
struct SMSC95xxDriver_IVars {
    IOUSBHostInterface      *interface;
    IOBufferMemoryDescriptor *ctrlBuffer;
    uint8_t                 *ctrlBytes;
    /* `interface` is non-null from the moment the provider is cast, i.e. before it is
     * opened, and it stays non-null after a failed Open. Closing in that state would
     * unbalance IOKit's opener accounting, so track the two facts separately. */
    bool                     interfaceOpened;
    /* The stack fetches the MAC through getHardwareAddress() rather than as a
     * registration argument, so the validated bytes have to outlive Start(). */
    uint8_t                  macAddress[SMSC95XX_MAC_LEN];
    bool                     macValid;
    IODispatchQueue                *dispatchQueue;
    IOUserNetworkPacketBufferPool  *pool;
    IOUserNetworkTxSubmissionQueue *txSubmit;
    IOUserNetworkRxSubmissionQueue *rxSubmit;
    IOUserNetworkTxCompletionQueue *txComplete;
    IOUserNetworkRxCompletionQueue *rxComplete;

    /* Datapath. One transfer in flight in each direction -- M5 is correctness, M6 is
     * throughput (AsyncIOBundled is the upgrade path). */
    IOUSBHostPipe            *pipeIn;      /* bulk IN  0x81, retained by CopyPipe */
    IOUSBHostPipe            *pipeOut;     /* bulk OUT 0x02, retained by CopyPipe */
    IOBufferMemoryDescriptor *rxBuffer;
    uint8_t                  *rxBytes;
    IOBufferMemoryDescriptor *txBuffer;
    uint8_t                  *txBytes;
    OSAction                 *rxAction;
    OSAction                 *txAction;
    /* The TX doorbell. txDataQueue is the TX submission queue's shared-memory data queue,
     * obtained with CopyDataQueue -- Copy, so it arrives retained and is ours to release.
     * txDataAvailableAction carries TxDataAvailable; the source retains it too, but our
     * own reference still has to be dropped. */
    IODataQueueDispatchSource *txDataQueue;
    OSAction                 *txDataAvailableAction;
    /* True from the moment AsyncIO on the bulk OUT pipe is accepted until its completion
     * runs. STATE, not a diagnostic: there is exactly one TX scratch buffer, so a second
     * overlapping transfer would frame a new packet over the bytes the first one is still
     * sending. The drain loop refuses to consume anything while this is set, which leaves
     * the packets in the submission queue rather than dropping them, and TxComplete clears
     * it before doing anything else. */
    bool                      txInFlight;
    IOUserNetworkPacket      *txPacket;    /* the packet the in-flight TX belongs to */
    /* True between the end of setupDatapath() and the FIRST statement of
     * teardownDatapath(), and it is the drain loop's gate. teardownDatapath aborts the bulk
     * OUT pipe synchronously, and the aborted transfer's TxComplete can be delivered while
     * that abort is still on the stack -- so without this, that completion would pull fresh
     * packets out of the submission queue and hand them to a pipe and a buffer that are
     * about to be released. With it, the aborted completion still returns its own packet to
     * the stack (the TX completion queue outlives teardownDatapath: TearDown releases the
     * queues only after it returns) and then stops. */
    bool                      txDatapathReady;
    /* Diagnostic, not synchronisation. The design assumes every datapath callback
     * serialises on one dispatch queue and therefore needs no locking. That should now
     * hold by construction -- a DataAvailable handler runs on the queue set for the target
     * method of its OSAction, which for TxDataAvailable is the same queue the pipe
     * completions use -- so this flag exists to check the load-bearing assumption rather
     * than to assume it. Set on entry to TxDataAvailable and cleared on every exit: if it
     * is already set on entry, callbacks are NOT serialised and the design has to be
     * revisited rather than patched with a lock. Being a plain bool it can miss a genuine
     * race, so a clean run is weak evidence, not proof. */
    bool                      inTxDataAvailable;
    /* The RX half of the same diagnostic. Set on entry to RxComplete and cleared on every
     * exit; a true reading on entry means the completion handler re-entered, i.e. the
     * "one dispatch queue, therefore no locking" assumption is void. Same caveat as
     * inTxDataAvailable: a plain bool can miss a genuine race, so quiet is weak evidence. */
    bool                      inRxComplete;
    /* The TX-completion half of the same diagnostic, which the datapath file asked for when
     * TxComplete grew a body. Note the ONE expected exception, handled in TxComplete rather
     * than here: the completion of a transfer aborted by teardownDatapath is delivered on
     * whichever thread called Abort(kIOUSBAbortSynchronous), which is not necessarily the
     * driver's dispatch queue -- so the OnQueue() check is skipped once txDatapathReady is
     * false, or teardown would print a spurious "not serialised" line every time. */
    bool                      inTxComplete;
    /* True while a bulk IN transfer is outstanding against ivars->rxBuffer. One buffer means
     * one transfer: two overlapping AsyncIOs would write the same memory, so armRxRead()
     * refuses to arm when this is set and RxComplete clears it before doing anything else. */
    bool                      rxArmed;
    /* True between setInterfaceEnable(true) and setInterfaceEnable(false)/teardown. RX is
     * only re-armed while this holds, so disabling the interface lets the read loop wind
     * down after at most one more completion instead of reading into a released buffer. */
    bool                      rxRunning;

    /* The idle-RX backoff. Without it, a completion that delivers nothing is re-armed
     * immediately, and a device with nothing to give completes the next read instantly:
     * measured at ~364,000 arms and 19% of a CPU core. This is STATE, not a diagnostic --
     * RxComplete branches on it.
     *
     * rxIdleRun counts CONSECUTIVE completions that delivered no bytes: a zero-length
     * success or a failed transfer both count, because both leave nothing to hand the
     * stack and both can repeat instantly. Any completion carrying data resets it to 0,
     * which is what returns the receive path to immediate re-arming.
     *
     * rxBackoffTimer re-arms the read after SMSC95XX_RX_BACKOFF_NS once the run passes
     * SMSC95XX_RX_IDLE_RUN_LIMIT, so the loop settles at a handful of arms per second
     * instead of spinning, and the interface is never left permanently deaf.
     * rxBackoffActive exists so the log line is emitted once per episode rather than once
     * per completion -- the busy loop being diagnosed produced its own log flood. */
    uint64_t                  rxIdleRun;
    bool                      rxBackoffActive;
    IOTimerDispatchSource    *rxBackoffTimer;
    OSAction                 *rxBackoffAction;
    uint64_t                  rxBackoffEpisodes;    /* times the backoff engaged      */
    uint64_t                  rxBackoffWakes;       /* timer callbacks delivered      */
    uint64_t                  rxFrames;
    uint64_t                  rxDropped;
    uint64_t                  txFrames;             /* transfers that completed successfully */
    uint64_t                  txErrors;
    /* TX diagnostics (M5). Counters, not state: nothing branches on them.
     *
     * THE ONE THAT MATTERS IS THE PAIR txDequeued / (txReturned + txLost). Every packet
     * taken off the submission queue must be handed back to the stack exactly once, so
     * txDequeued == txReturned + txLost + (1 if a transfer is in flight else 0) at every
     * quiescent point. A shortfall means packets are being consumed and never completed,
     * which drains the 64-packet pool and wedges transmit silently -- so this is the
     * accounting identity to read out of the log, not a nice-to-have. */
    uint64_t                  txDoorbells;          /* TxDataAvailable callbacks           */
    uint64_t                  txDrains;             /* drain-loop calls, either caller     */
    uint64_t                  txDequeued;           /* packets taken off txSubmit          */
    uint64_t                  txSubmitted;          /* AsyncIO submissions that succeeded  */
    uint64_t                  txCompletions;        /* TxComplete callbacks, all statuses  */
    uint64_t                  txReturned;           /* packets handed back to the stack    */
    uint64_t                  txLost;               /* packets neither sent nor returned   */
    uint64_t                  txBytesSent;          /* bytes the bulk OUT pipe reported    */
    uint64_t                  txRejected;           /* packets refused before framing      */
    uint64_t                  txSubmitFailures;     /* AsyncIO submissions that failed     */
    uint64_t                  txStalls;             /* bulk OUT stalls cleared             */
    uint64_t                  txAborted;            /* completions with kIOReturnAborted   */
    uint64_t                  txDeferred;           /* drains that stopped on txInFlight   */
    /* RX diagnostics (M5). Counters, not state: nothing branches on them, they exist so a
     * single log line can say how far the receive path got. */
    uint64_t                  rxCompletions;        /* RxComplete callbacks, all statuses  */
    uint64_t                  rxCompletionErrors;   /* completions with a failure status   */
    uint64_t                  rxZeroLength;         /* successful completions of 0 bytes   */
    uint64_t                  rxByteCount;          /* bytes delivered by the bulk IN pipe */
    uint64_t                  rxRecords;            /* status-word records decoded         */
    uint64_t                  rxEnqueued;           /* packets accepted by the stack       */
    uint64_t                  rxEnqueueFailures;    /* enqueuePackets calls that failed    */
    /* rxSubmitDequeued vs rxSubmitEmpty is the load-bearing pair: it says whether the stack
     * supplies receive buffers on the RX submission queue at all, which is the difference
     * between the working model and falling back to a pool allocation the stack will refuse
     * to accept. rxPoolFailures then counts the fallback failing too. */
    uint64_t                  rxSubmitDequeued;     /* buffers taken from rxSubmit         */
    uint64_t                  rxSubmitEmpty;        /* times rxSubmit had nothing to give  */
    uint64_t                  rxPoolFailures;       /* fallback allocatePacket failures    */
    uint64_t                  rxLost;               /* packets neither delivered nor freed */
    uint64_t                  rxArmCount;           /* successful AsyncIO submissions      */
    uint64_t                  rxArmFailures;        /* AsyncIO submissions that failed     */
};

#endif /* SMSC95xxDriver_ivars_h */
