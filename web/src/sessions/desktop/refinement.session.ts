import type { ultraterimal } from "../../generated/session";
import { DesktopTiles, desktopTileAreas } from "./tiles.session";
import type {
  DesktopVideoMessageReservation,
  DesktopAuxiliaryLease,
} from "./video-queue.session";
import { videoDecodeSlotTimeoutMilliseconds } from "./video-queue.session";

export function desktopDuration(milliseconds: number): number {
  return Math.min(60000000, Math.max(0, Math.round(milliseconds * 1000)));
}

// A request is not activation: only a subsequent VideoConfig barrier can
// activate the exact three-party contract. Unknown versions are legacy.
export class DesktopOptimizationContract {
  private control = 0;
  private video = 0;
  private agent = 0;
  private requested = false;

  ready(purpose: 2 | 3, contract: number): void {
    if (purpose === 2) this.control = contract;
    else this.video = contract;
  }

  configure(contract: number): boolean {
    const active =
      this.requested &&
      this.control === 2 &&
      this.video === 2 &&
      contract === 2;
    this.agent = contract;
    return active;
  }

  request(): number {
    const contract =
      this.control === 2 && this.video === 2 && this.agent === 2 ? 2 : 0;
    this.requested = contract === 2;
    return contract;
  }
}

export interface DesktopRefinementState {
  generation: number;
  frame: number;
  displayedFrame: number;
  identity: object | null;
  unavailable: boolean;
  background: boolean;
}
export interface DesktopRefinementBinding {
  generation: number;
  frame: number;
  batch: bigint;
  kind: number;
  receivedAt: number;
}
type Token = DesktopRefinementBinding & {
  identity: object | null;
  retired: boolean;
  lease: DesktopAuxiliaryLease;
  wake?: (ready: boolean) => void;
  timer?: ReturnType<typeof setTimeout>;
};

// Admission state survives retirement so queued cleanup uses the same batch,
// geometry and cumulative-budget gates as a batch that starts native decoding.
export class DesktopRefinementAdmission {
  private lastBatch = 0n;
  private baseline = 0;
  private grid = new Set<number>();
  private bytes = 0;

  constructor(readonly generation: number) {}

  validate(
    binding: DesktopRefinementBinding,
    patches: ultraterimal.wire.IPatchRect[],
    width: number,
    height: number,
    frame: number,
  ): void {
    if (
      binding.generation !== this.generation ||
      binding.frame !== frame ||
      !binding.frame ||
      binding.batch <= this.lastBatch ||
      binding.batch > 0xffffffffffffffffn ||
      (binding.kind !== 1 && binding.kind !== 2)
    )
      throw new Error("Invalid desktop refinement binding or batch");
    const { areas, bytes } = desktopTileAreas(patches, width, height, true);
    const columns = Math.ceil(width / 128);
    const grid =
      binding.frame === this.baseline ? this.grid : new Set<number>();
    const previousBytes = binding.frame === this.baseline ? this.bytes : 0;
    const indices = new Set<number>();
    for (const area of areas) {
      const index = (area.y / 64) * columns + area.x / 128;
      if (
        area.x % 128 ||
        area.y % 64 ||
        area.width !== Math.min(128, width - area.x) ||
        area.height !== Math.min(64, height - area.y) ||
        grid.has(index) ||
        indices.has(index)
      )
        throw new Error("Invalid or duplicate desktop refinement grid tile");
      indices.add(index);
    }
    if (
      grid.size + indices.size >
        Math.min(2048, columns * Math.ceil(height / 64)) ||
      previousBytes + bytes > 8 * 1024 * 1024
    )
      throw new Error("Desktop refinement baseline budget exceeded");
    this.baseline = binding.frame;
    this.grid = grid;
    for (const index of indices) grid.add(index);
    this.bytes = previousBytes + bytes;
    this.lastBatch = binding.batch;
  }
}

// One task for the entire session, including retired native decodes. Never
// reset this owner on generation changes: retirement is not cancellation.
export class DesktopRefinements {
  private active: Token | undefined;
  private admission = new DesktopRefinementAdmission(0);

  constructor(
    private readonly tiles: DesktopTiles,
    private readonly state: () => DesktopRefinementState,
    private readonly draw: (
      tiles: Awaited<ReturnType<DesktopTiles["prepare"]>>["tiles"],
    ) => void,
    private readonly ack: (
      binding: DesktopRefinementBinding,
      applied: boolean,
    ) => void,
    private readonly applied: (
      binding: DesktopRefinementBinding,
      timing: ultraterimal.wire.IDesktopBrowserTiming,
      bytes: number,
    ) => void,
    private readonly failed: (error: unknown) => void,
    private readonly now: () => number = () => performance.now(),
  ) {}

  beginGeneration(generation: number): DesktopRefinementAdmission {
    this.retire();
    this.admission = new DesktopRefinementAdmission(generation);
    return this.admission;
  }

  supersede(
    binding: DesktopRefinementBinding,
    patches: ultraterimal.wire.IPatchRect[],
    width: number,
    height: number,
    frame: number,
    admission: DesktopRefinementAdmission,
    reservation: DesktopVideoMessageReservation,
  ): void {
    admission.validate(binding, patches, width, height, frame);
    reservation.owner.releaseMessageBytes(reservation);
    this.ack(binding, false);
  }

  retire(): void {
    const token = this.active;
    if (!token) return;
    token.retired = true;
    clearTimeout(token.timer);
    token.wake?.(false);
    token.wake = undefined;
  }

  baselineChanged(): void {
    const token = this.active;
    if (!token) return;
    if (!this.current(token)) this.retire();
    else if (this.state().displayedFrame === token.frame) {
      token.wake?.(true);
      token.wake = undefined;
    }
  }

  accept(
    binding: DesktopRefinementBinding,
    patches: ultraterimal.wire.IPatchRect[],
    width: number,
    height: number,
    reservation: DesktopVideoMessageReservation,
  ): void {
    const state = this.state();
    if (binding.generation !== state.generation || !state.identity)
      throw new Error("Invalid desktop refinement binding or batch");
    this.admission.validate(binding, patches, width, height, state.frame);
    if (
      this.active ||
      state.unavailable ||
      (binding.kind === 2 &&
        (state.background || this.now() - binding.receivedAt >= 50))
    ) {
      reservation.owner.releaseMessageBytes(reservation);
      this.ack(binding, false);
      return;
    }
    const token: Token = {
      ...binding,
      identity: state.identity,
      retired: false,
      lease: reservation.owner.transferAuxiliary(reservation),
    };
    this.active = token;
    if (token.kind === 2)
      token.timer = setTimeout(
        () => this.retire(),
        Math.max(0, 50 - (this.now() - token.receivedAt)),
      );
    void this.run(token, patches, width, height);
  }

  private current(token: Token): boolean {
    const state = this.state();
    return (
      this.active === token &&
      !token.retired &&
      !state.unavailable &&
      state.identity === token.identity &&
      state.generation === token.generation &&
      state.frame === token.frame &&
      state.displayedFrame <= token.frame &&
      (token.kind !== 2 ||
        (!state.background && this.now() - token.receivedAt < 50))
    );
  }

  private async run(
    token: Token,
    patches: ultraterimal.wire.IPatchRect[],
    width: number,
    height: number,
  ): Promise<void> {
    let batch: Awaited<ReturnType<DesktopTiles["prepare"]>> | undefined;
    let applied = false;
    let failure: unknown;
    let timing: ultraterimal.wire.IDesktopBrowserTiming | undefined;
    try {
      const pngStarted = this.now();
      const receiveQueueMicroseconds = desktopDuration(
        pngStarted - token.receivedAt,
      );
      batch = await this.tiles.prepare(patches, width, height, {
        isCurrent: () => this.current(token),
      });
      const pngDecodeMicroseconds = desktopDuration(this.now() - pngStarted);
      let baselineWaitMicroseconds: number | undefined;
      if (this.current(token) && this.state().displayedFrame < token.frame) {
        const waitStarted = this.now();
        const ready = await new Promise<boolean>((resolve) => {
          token.wake = resolve;
          // Dynamic expiry has its own timer and retires this same token.
          const timer = setTimeout(
            () => resolve(false),
            videoDecodeSlotTimeoutMilliseconds,
          );
          token.wake = (ready) => {
            clearTimeout(timer);
            resolve(ready);
          };
        });
        token.wake = undefined;
        // Refinement is optional. A baseline that did not arrive in time
        // supersedes this batch without ending the normal video pipeline.
        if (!ready) return;
        baselineWaitMicroseconds = desktopDuration(this.now() - waitStarted);
      }
      // No await between this identity/frame/generation check and synchronous
      // canvas/cache submission. These durations do not measure physical display.
      if (!this.current(token) || this.state().displayedFrame !== token.frame)
        return;
      const submitStarted = this.now();
      this.draw(batch.tiles);
      batch.commit();
      timing = {
        receiveQueueMicroseconds,
        pngDecodeMicroseconds,
        ...(baselineWaitMicroseconds !== undefined
          ? { baselineWaitMicroseconds }
          : {}),
        canvasSubmitMicroseconds: desktopDuration(this.now() - submitStarted),
      };
      applied = true;
    } catch (error) {
      if (this.current(token)) failure = error;
    } finally {
      batch?.dispose();
      clearTimeout(token.timer);
      token.wake?.(false);
      token.lease.release();
      if (this.active === token) this.active = undefined;
      // Independent, non-coalescible ACK; release every bitmap and lease first.
      this.ack(token, applied);
      if (applied && timing) this.applied(token, timing, batch?.bytes ?? 0);
      if (failure !== undefined) this.failed(failure);
    }
  }
}
