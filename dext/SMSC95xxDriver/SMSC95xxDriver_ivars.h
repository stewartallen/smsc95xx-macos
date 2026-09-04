/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SMSC95xxDriver's instance variables.
 *
 * The class is built from two translation units -- SMSC95xxDriver.cpp owns the control
 * path (registers, MII, EEPROM, chip init, network-stack plumbing) and
 * SMSC95xxDriver_datapath.cpp owns the bulk endpoints -- and both dereference `ivars`, so
 * the layout is one shared definition. iig only forward-declares SMSC95xxDriver_IVars in
 * the generated header; defining it is ours to do, exactly once.
 *
 * Pointer members are forward-declared rather than #included: nothing here calls a
 * method, and each .cpp includes the real headers for the objects it touches.
 */

#ifndef SMSC95xxDriver_ivars_h
#define SMSC95xxDriver_ivars_h

#include <stdint.h>

#include "smsc95xx_regs.h"      /* SMSC95XX_MAC_LEN */

/* The packet pool shared by both directions, created in SMSC95xxDriver.cpp and used as
 * a memcpy bound in SMSC95xxDriver_datapath.cpp -- defined once here so the pool's real
 * geometry and the bounds checks against it cannot drift apart. 2048 bytes comfortably
 * holds a 1518-byte maximum frame plus the 8-byte TX command header at any data offset
 * the family reserves. */
#define SMSC95XX_POOL_PACKET_COUNT 64
#define SMSC95XX_POOL_BUFFER_SIZE  2048

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
 * handle: DeviceRequest with kIOUSBDeviceRequestRecipientDevice carries vendor control
 * transfers to the device (see readRegister/writeRegister), and the datapath takes its
 * pipes from the interface. Configuration selection is SMSC95xxUSBDevice's job, and
 * IOUSBHostDevice admits one opener which that driver holds -- so a CopyDevice() handle
 * here could not be opened anyway.
 * `ctrlBuffer` is a 4-byte IOBufferMemoryDescriptor for all vendor control transfers;
 * `ctrlBytes` is its mapped address. */
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

    /* Datapath. One transfer in flight in each direction. */
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
    /* True between the end of setupDatapath() and the first statement of
     * quiesceDatapath(); the drain loop's gate. quiesceDatapath aborts the bulk OUT pipe
     * synchronously, and the aborted transfer's TxComplete can be delivered while that
     * abort is still on the stack -- with this flag false that completion returns its own
     * packet (the TX completion queue is still alive, being released only after every
     * cancellation has run) and then stops, rather than pulling fresh packets into a
     * buffer about to be released. */
    bool                      txDatapathReady;
    /* Re-entrancy guards on the "every datapath callback serialises on one dispatch queue,
     * so no locking" assumption. Set on entry to each callback and cleared on exit; a set
     * flag on entry means callbacks are NOT serialised and the design must be revisited,
     * not patched with a lock. Plain bools, so a clean run is weak evidence, not proof.
     * inTxComplete's OnQueue() check is skipped once txDatapathReady is false: the aborted
     * completion at teardown is delivered off the dispatch queue and would fire it. */
    bool                      inTxDataAvailable;
    bool                      inRxComplete;
    bool                      inTxComplete;
    /* True while a bulk IN transfer is outstanding against ivars->rxBuffer. One buffer means
     * one transfer: two overlapping AsyncIOs would write the same memory, so armRxRead()
     * refuses to arm when this is set and RxComplete clears it before doing anything else. */
    bool                      rxArmed;
    /* True between setInterfaceEnable(true) and setInterfaceEnable(false)/teardown. RX is
     * only re-armed while this holds, so disabling the interface lets the read loop wind
     * down after at most one more completion instead of reading into a released buffer. */
    bool                      rxRunning;
    /* Latched true by quiesceDatapath and never cleared: teardown has begun. Stop() can
     * return with its cancellation handlers still pending, and those handlers release the
     * buffers -- so a setInterfaceEnable(true) or armRxRead delivered in that gap must
     * refuse rather than arm I/O into memory about to be freed. */
    bool                      stopping;
    /* Cancellations Stop() has issued whose handlers have not yet run. The shared
     * cancellation handler counts it down; whichever handler reaches zero releases the
     * driver's resources and calls super::Stop. Lives here rather than on Stop's stack
     * because the handlers run after Stop has returned. */
    _Atomic uint32_t          stopCancelsPending;

    /* The idle-RX backoff state (RxComplete branches on it). rxIdleRun counts consecutive
     * completions that delivered no bytes -- a zero-length success or a failed transfer
     * both count; a completion carrying data resets it to 0. Past SMSC95XX_RX_IDLE_RUN_LIMIT,
     * rxBackoffTimer re-arms the read every SMSC95XX_RX_BACKOFF_NS so an idle device settles
     * at a few arms per second instead of spinning. rxBackoffActive gates the log line to
     * once per episode. */
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
    /* TX counters, not state: nothing branches on them. The load-bearing pair is
     * txDequeued vs (txReturned + txLost): every packet taken off the submission queue is
     * handed back exactly once, so txDequeued == txReturned + txLost + (1 if in flight) at
     * every quiescent point. A shortfall means packets consumed and never completed, which
     * drains the pool and wedges transmit silently. */
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
    /* RX counters, not state: they exist so a single log line can say how far the receive
     * path got. records == frames + dropped is the identity. */
    uint64_t                  rxCompletions;        /* RxComplete callbacks, all statuses  */
    uint64_t                  rxCompletionErrors;   /* completions with a failure status   */
    uint64_t                  rxZeroLength;         /* successful completions of 0 bytes   */
    uint64_t                  rxByteCount;          /* bytes delivered by the bulk IN pipe */
    uint64_t                  rxRecords;            /* status-word records decoded         */
    uint64_t                  rxZeroRecords;        /* transfers with bytes but no records */
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
