export const maximumPendingVideoFrames = 128;
export const maximumVideoQueueMessages = maximumPendingVideoFrames + 2;
export const maximumControlQueueMessages = 64;
export const maximumDesktopMessageBytes = 16 * 1024 * 1024;
export const maximumDesktopReceiveBytes = 32 * 1024 * 1024;
// Keep AVC dependencies buffered on the wire, with a small decoder pipeline.
export const maximumSubmittedDecodeFrames = 4;
export const videoDecodeSlotTimeoutMilliseconds = 5000;

export class DesktopVideoDecodeTimeoutError extends Error {
  constructor() {
    super("Timed out waiting for a video decode slot");
    this.name = "DesktopVideoDecodeTimeoutError";
  }
}

type DecodeSlotResult =
  | { lease: DesktopVideoDecodeLease }
  | { reason: "closed" | "reconfigured" | "timeout" };
type DecodeWaiter = {
  resolve: (result: DecodeSlotResult) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class DesktopVideoDecodeLease {
  private released = false;

  constructor(
    private readonly slots: DesktopVideoDecodeSlots,
    private readonly generation: number,
  ) {}

  release(): void {
    if (this.released) return;
    this.released = true;
    this.slots.release(this.generation);
  }
}

class DesktopVideoDecodeSlots {
  private active = 0;
  private generation = 0;
  private closed = false;
  private waiters: DecodeWaiter[] = [];

  acquire(timeoutMilliseconds: number): Promise<DecodeSlotResult> {
    if (this.closed) return Promise.resolve({ reason: "closed" });
    if (this.active < maximumSubmittedDecodeFrames) {
      this.active++;
      return Promise.resolve({
        lease: new DesktopVideoDecodeLease(this, this.generation),
      });
    }
    return new Promise((resolve) => {
      const waiter: DecodeWaiter = {
        resolve,
        timer: setTimeout(() => {
          this.removeWaiter(waiter);
          resolve({ reason: "timeout" });
        }, timeoutMilliseconds),
      };
      this.waiters.push(waiter);
    });
  }

  release(generation: number): void {
    if (this.closed || generation !== this.generation) return;
    const waiter = this.waiters.shift();
    if (waiter) {
      clearTimeout(waiter.timer);
      waiter.resolve({
        lease: new DesktopVideoDecodeLease(this, generation),
      });
      return;
    }
    this.active = Math.max(0, this.active - 1);
  }

  interruptWaiters(): void {
    this.generation++;
    this.active = 0;
    this.resolveWaiters("reconfigured");
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.generation++;
    this.active = 0;
    this.resolveWaiters("closed");
  }

  get waiterCount(): number {
    return this.waiters.length;
  }

  private removeWaiter(waiter: DecodeWaiter): void {
    const index = this.waiters.indexOf(waiter);
    if (index !== -1) this.waiters.splice(index, 1);
  }

  private resolveWaiters(reason: "closed" | "reconfigured"): void {
    for (const waiter of this.waiters) {
      clearTimeout(waiter.timer);
      waiter.resolve({ reason });
    }
    this.waiters = [];
  }
}

export interface DesktopVideoMessageReservation {
  readonly owner: DesktopVideoReceiveBudget;
  remainingBytes: number;
  active: boolean;
}

export class DesktopVideoReceiveBudget {
  private queuedMessages = 0;
  private queuedBytes = 0;
  private pendingFrames = new Map<number, number>();
  private pendingBytes = 0;

  constructor(private readonly video: boolean) {}

  reserveMessage(bytes: number): DesktopVideoMessageReservation | null {
    if (
      !Number.isSafeInteger(bytes) ||
      bytes < 0 ||
      bytes > maximumDesktopMessageBytes ||
      this.queuedMessages + 1 >
        (this.video
          ? maximumVideoQueueMessages
          : maximumControlQueueMessages) ||
      this.queuedBytes + this.pendingBytes + bytes > maximumDesktopReceiveBytes
    )
      return null;
    this.queuedMessages++;
    this.queuedBytes += bytes;
    return { owner: this, remainingBytes: bytes, active: true };
  }

  finishMessage(reservation: DesktopVideoMessageReservation): void {
    if (
      reservation.owner !== this ||
      !reservation.active ||
      this.queuedMessages <= 0 ||
      reservation.remainingBytes > this.queuedBytes
    )
      throw new Error("Desktop receive queue accounting underflow");
    this.queuedMessages--;
    this.queuedBytes -= reservation.remainingBytes;
    reservation.remainingBytes = 0;
    reservation.active = false;
  }

  reservePendingFrame(
    timestamp: number,
    bytes: number,
    message: DesktopVideoMessageReservation,
  ): boolean {
    if (
      !this.video ||
      message.owner !== this ||
      !message.active ||
      !Number.isSafeInteger(timestamp) ||
      timestamp < 0 ||
      !Number.isSafeInteger(bytes) ||
      bytes <= 0 ||
      bytes > maximumDesktopMessageBytes ||
      bytes > message.remainingBytes ||
      this.pendingFrames.size >= maximumSubmittedDecodeFrames ||
      this.pendingFrames.has(timestamp) ||
      this.queuedBytes - message.remainingBytes + this.pendingBytes + bytes >
        maximumDesktopReceiveBytes
    )
      return false;
    // Atomically transfer this message's byte reservation to the decoder-pending
    // budget; its message-count reservation remains until channel.work completes.
    this.queuedBytes -= message.remainingBytes;
    message.remainingBytes = 0;
    this.pendingFrames.set(timestamp, bytes);
    this.pendingBytes += bytes;
    return true;
  }

  finishPendingFrame(timestamp: number): boolean {
    const bytes = this.pendingFrames.get(timestamp);
    if (bytes === undefined) return false;
    this.pendingFrames.delete(timestamp);
    this.pendingBytes -= bytes;
    return true;
  }

  clearPendingFrames(): void {
    this.pendingFrames.clear();
    this.pendingBytes = 0;
  }

  get queuedMessageCount(): number {
    return this.queuedMessages;
  }

  get queuedMessageBytes(): number {
    return this.queuedBytes;
  }

  get pendingFrameCount(): number {
    return this.pendingFrames.size;
  }

  get pendingFrameBytes(): number {
    return this.pendingBytes;
  }
}

// Owns the existing per-channel Promise chain and its admission budgets. Messages
// stay in channel.work while a decoder slot is unavailable; no second frame queue
// or encoded-payload copy is introduced here.
export class DesktopVideoReceiveChannel {
  readonly budget: DesktopVideoReceiveBudget;
  private work: Promise<void> = Promise.resolve();
  private slots: DesktopVideoDecodeSlots | null;
  private closed = false;
  private reconfiguringGeneration = 0;

  constructor(private readonly video: boolean) {
    this.budget = new DesktopVideoReceiveBudget(video);
    this.slots = video ? new DesktopVideoDecodeSlots() : null;
  }

  enqueue(
    data: ArrayBuffer,
    receive: (reservation: DesktopVideoMessageReservation) => Promise<void>,
    failed: (error: unknown) => void,
    queued?: () => void,
  ): boolean {
    if (this.closed) return false;
    const reservation = this.budget.reserveMessage(data.byteLength);
    if (!reservation) return false;
    try {
      queued?.();
    } catch (error) {
      this.budget.finishMessage(reservation);
      throw error;
    }
    this.work = this.work
      .then(() => receive(reservation))
      .catch(failed)
      .finally(() => this.budget.finishMessage(reservation));
    return true;
  }

  async submitFrame(
    reservation: DesktopVideoMessageReservation,
    timestamp: number,
    bytes: number,
    timeoutMilliseconds: number,
    isCurrent: () => boolean,
    submit: (lease: DesktopVideoDecodeLease) => void,
  ): Promise<"submitted" | "cancelled"> {
    if (!this.slots || !this.video)
      throw new Error("Video decode slots are unavailable");
    const result = await this.slots.acquire(timeoutMilliseconds);
    if ("reason" in result) {
      if (result.reason !== "timeout") return "cancelled";
      throw new DesktopVideoDecodeTimeoutError();
    }
    const { lease } = result;
    if (!isCurrent()) {
      lease.release();
      return "cancelled";
    }
    if (!this.budget.reservePendingFrame(timestamp, bytes, reservation)) {
      lease.release();
      throw new Error("Desktop video decoder queue exceeded its limits");
    }
    try {
      submit(lease);
      return "submitted";
    } catch (error) {
      this.budget.finishPendingFrame(timestamp);
      lease.release();
      throw error;
    }
  }

  finishFrame(timestamp: number, lease: DesktopVideoDecodeLease): boolean {
    const finished = this.budget.finishPendingFrame(timestamp);
    lease.release();
    return finished;
  }

  announceVideoGeneration(generation: number, currentGeneration: number): void {
    if (!this.video || generation <= currentGeneration) return;
    this.reconfiguringGeneration = Math.max(
      this.reconfiguringGeneration,
      generation,
    );
    if (this.decodeWaiterCount > 0) this.slots?.interruptWaiters();
  }

  isRetiredGeneration(generation: number, currentGeneration: number): boolean {
    return (
      this.video &&
      this.reconfiguringGeneration > currentGeneration &&
      generation < this.reconfiguringGeneration
    );
  }

  get isReconfiguring(): boolean {
    return this.reconfiguringGeneration > 0;
  }

  beginVideoGeneration(generation: number): void {
    if (!this.video) return;
    this.reconfiguringGeneration = Math.max(
      this.reconfiguringGeneration,
      generation,
    );
    this.resetVideoGeneration();
  }

  completeVideoGeneration(generation: number): void {
    if (this.reconfiguringGeneration <= generation)
      this.reconfiguringGeneration = 0;
  }

  whenIdle(): Promise<void> {
    return this.work;
  }

  resetVideoGeneration(): void {
    this.slots?.interruptWaiters();
    this.budget.clearPendingFrames();
  }

  close(): void {
    this.closed = true;
    this.slots?.close();
    this.budget.clearPendingFrames();
  }

  get decodeWaiterCount(): number {
    return this.slots?.waiterCount ?? 0;
  }
}
