import { expect, test } from "bun:test";
import {
  DesktopRefinements,
  DesktopOptimizationContract,
  desktopDuration,
  type DesktopRefinementBinding,
  type DesktopRefinementState,
} from "../../web/src/sessions/desktop/refinement.session";
import { DesktopTiles } from "../../web/src/sessions/desktop/tiles.session";
import {
  DesktopVideoReceiveChannel,
  maximumDesktopReceiveBytes,
  maximumDesktopMessageBytes,
} from "../../web/src/sessions/desktop/video-queue.session";
import type { ultraterimal } from "../../web/src/generated/session";

function png(): Uint8Array {
  const bytes = new Uint8Array(33);
  const view = new DataView(bytes.buffer);
  for (const [offset, value] of [
    [0, 0x89504e47],
    [4, 0x0d0a1a0a],
    [8, 13],
    [12, 0x49484452],
    [16, 128],
    [20, 64],
  ])
    view.setUint32(offset, value);
  return bytes;
}
const patches = (count = 8, offset = 0) =>
  Array.from({ length: count }, (_, index) => ({
    destination: {
      x: ((offset + index) % 32) * 128,
      y: Math.floor((offset + index) / 32) * 64,
      width: 128,
      height: 64,
    },
    png: png(),
    cacheSlot: 0,
  }));
async function until(predicate: () => boolean): Promise<void> {
  for (let i = 0; i < 100 && !predicate(); i++)
    await new Promise((resolve) => setTimeout(resolve, 0));
  expect(predicate()).toBe(true);
}
function fixture() {
  const channel = new DesktopVideoReceiveChannel(true);
  const state: DesktopRefinementState = {
    generation: 1,
    frame: 1,
    displayedFrame: 1,
    identity: {},
    unavailable: false,
    background: false,
  };
  const releases: (() => void)[] = [];
  let started = 0,
    active = 0,
    peak = 0,
    closed = 0,
    draws = 0,
    clock = 100;
  const acknowledgements: {
    generation: number;
    frame: number;
    batch: bigint;
    applied: boolean;
    charged: number;
    active: number;
  }[] = [];
  const timings: ultraterimal.wire.IDesktopBrowserTiming[] = [];
  const failures: unknown[] = [];
  const tiles = new DesktopTiles(async () => {
    started++;
    active++;
    peak = Math.max(peak, active);
    await new Promise<void>((resolve) => releases.push(resolve));
    active--;
    return {
      width: 128,
      height: 64,
      close: () => {
        closed++;
      },
    };
  });
  const refinement = new DesktopRefinements(
    tiles,
    () => state,
    (tiles) => {
      draws += tiles.length;
    },
    (binding, applied) => {
      acknowledgements.push({
        ...binding,
        applied,
        charged: channel.budget.auxiliaryBytes,
        active,
      });
    },
    (_binding, timing) => timings.push(timing),
    (error) => failures.push(error),
    () => clock,
  );
  refinement.beginGeneration(1);
  let offset = 0;
  const enqueue = (
    batch = 1n,
    count = 8,
    kind = 1,
    bytes = 1024,
    receivedAt = clock,
  ) =>
    channel.enqueue(
      new ArrayBuffer(bytes),
      async (reservation) => {
        const binding: DesktopRefinementBinding = {
          generation: state.generation,
          frame: state.frame,
          batch,
          kind,
          receivedAt,
        };
        refinement.accept(
          binding,
          patches(count, offset),
          4096,
          128,
          reservation,
        );
        offset += count;
      },
      (error) => failures.push(error),
    );
  const release = () => {
    for (const resolve of releases.splice(0)) resolve();
  };
  return {
    channel,
    state,
    refinement,
    tiles,
    enqueue,
    release,
    acknowledgements,
    timings,
    failures,
    setClock: (value: number) => {
      clock = value;
    },
    counts: () => ({ started, active, peak, closed, draws }),
  };
}

test("three exact-v2 gates require a requested capability and a subsequent generation barrier", () => {
  for (const control of [0, 1, 2, 3])
    for (const video of [0, 1, 2, 3])
      for (const agent of [0, 1, 2, 3]) {
        const gate = new DesktopOptimizationContract();
        gate.ready(2, control);
        gate.ready(3, video);
        expect(gate.configure(agent)).toBe(false);
        const enabled = control === 2 && video === 2 && agent === 2;
        expect(gate.request()).toBe(enabled ? 2 : 0);
        expect(gate.configure(agent)).toBe(enabled);
        expect(gate.configure(3)).toBe(false);
        expect(gate.request()).toBe(0);
      }
  expect(desktopDuration(0)).toBe(0);
  expect(desktopDuration(-10)).toBe(0);
  expect(desktopDuration(70000)).toBe(60000000);
});

test("slow v2 PNG leaves serial receive and new video admission free, with four native decodes maximum", async () => {
  const f = fixture();
  expect(f.enqueue()).toBe(true);
  await f.channel.whenIdle();
  expect(f.counts().active).toBe(4);
  expect(f.channel.budget.queuedMessageBytes).toBe(0);
  expect(f.channel.budget.auxiliaryBytes).toBe(1024);
  let video = false;
  f.channel.enqueue(
    new ArrayBuffer(512),
    async (reservation) => {
      f.state.frame = 2;
      f.refinement.retire();
      await f.channel.submitFrame(
        reservation,
        0,
        256,
        100,
        () => true,
        (lease) => {
          video = true;
          f.channel.finishFrame(0, lease);
        },
      );
    },
    (error) => f.failures.push(error),
  );
  await f.channel.whenIdle();
  expect(video).toBe(true);
  expect(f.counts().active).toBe(4);
  f.release();
  await until(() => f.acknowledgements.length === 1);
  expect(f.counts()).toEqual({
    started: 4,
    active: 0,
    peak: 4,
    closed: 4,
    draws: 0,
  });
  expect(f.acknowledgements[0]).toMatchObject({
    batch: 1n,
    applied: false,
    charged: 0,
    active: 0,
  });
  expect(f.failures).toEqual([]);
});

test("successful batches decode every patch under the ceiling and ACK only after bitmap/lease cleanup", async () => {
  const f = fixture();
  f.enqueue();
  await f.channel.whenIdle();
  f.release();
  await until(() => f.counts().started === 8);
  expect(f.counts().peak).toBe(4);
  f.setClock(110);
  f.release();
  await until(() => f.acknowledgements.length === 1);
  expect(f.counts()).toEqual({
    started: 8,
    active: 0,
    peak: 4,
    closed: 8,
    draws: 8,
  });
  expect(f.acknowledgements[0]).toMatchObject({
    generation: 1,
    frame: 1,
    batch: 1n,
    applied: true,
    charged: 0,
    active: 0,
  });
  expect(f.timings[0]).toEqual({
    receiveQueueMicroseconds: 0,
    pngDecodeMicroseconds: 10000,
    canvasSubmitMicroseconds: 0,
  });
  expect(Object.hasOwn(f.timings[0], "decodeMicroseconds")).toBe(false);
});

test("reconfigure, close and repeated retirement keep one live lease until late completion", async () => {
  for (const end of ["generation", "close", "retire"] as const) {
    const f = fixture();
    f.enqueue();
    await f.channel.whenIdle();
    if (end === "generation") {
      f.state.generation = 2;
      f.state.identity = {};
      f.refinement.beginGeneration(2);
      f.channel.beginVideoGeneration(2);
      f.channel.completeVideoGeneration(2);
      f.tiles.clear();
      f.enqueue(1n);
      await f.channel.whenIdle();
      expect(f.acknowledgements[0]).toMatchObject({
        generation: 2,
        applied: false,
      });
    } else if (end === "close") {
      f.state.unavailable = true;
      f.refinement.retire();
      f.channel.close();
      f.tiles.clear();
    } else {
      f.refinement.retire();
      f.refinement.retire();
      f.enqueue(2n);
      await f.channel.whenIdle();
      expect(f.acknowledgements[0]).toMatchObject({
        batch: 2n,
        applied: false,
      });
    }
    expect(f.channel.budget.auxiliaryBytes).toBe(1024);
    expect(f.counts().started).toBe(4);
    f.release();
    await until(() =>
      f.acknowledgements.some(
        (ack) => ack.generation === 1 && ack.batch === 1n,
      ),
    );
    expect(f.channel.budget.auxiliaryBytes).toBe(0);
    expect(f.counts().closed).toBe(4);
    expect(f.counts().draws).toBe(0);
    expect(f.failures).toEqual([]);
  }
});

test("auxiliary compressed bytes share the 32 MiB budget and release exactly once", async () => {
  const f = fixture();
  f.enqueue(1n, 8, 1, maximumDesktopMessageBytes);
  await f.channel.whenIdle();
  expect(f.channel.budget.auxiliaryBytes).toBe(maximumDesktopMessageBytes);
  const reservation = f.channel.budget.reserveMessage(
    maximumDesktopMessageBytes,
  );
  expect(reservation).not.toBeNull();
  expect(f.channel.budget.reserveMessage(1)).toBeNull();
  expect(
    f.channel.budget.auxiliaryBytes + f.channel.budget.queuedMessageBytes,
  ).toBe(maximumDesktopReceiveBytes);
  if (!reservation) throw new Error("Missing reservation");
  f.channel.budget.finishMessage(reservation);
  f.refinement.retire();
  f.release();
  await until(() => f.channel.budget.auxiliaryBytes === 0);
  const next = f.channel.budget.reserveMessage(100);
  if (!next) throw new Error("Missing reservation");
  const lease = f.channel.budget.transferAuxiliary(next);
  f.channel.budget.finishMessage(next);
  lease.release();
  lease.release();
  expect(f.channel.budget.auxiliaryBytes).toBe(0);
});

test("batch IDs are uint64, strictly increasing with gaps; malformed v2 slots never decode", async () => {
  const f = fixture();
  f.enqueue(9007199254740993n, 1);
  await f.channel.whenIdle();
  f.release();
  await until(() => f.acknowledgements.length === 1);
  for (const batch of [
    0n,
    9007199254740993n,
    9007199254740992n,
    0x10000000000000000n,
  ])
    f.enqueue(batch, 1);
  await f.channel.whenIdle();
  expect(f.failures).toHaveLength(4);
  f.enqueue(0xffffffffffffffffn, 1);
  await f.channel.whenIdle();
  f.release();
  await until(() => f.acknowledgements.length === 2);
  expect(f.acknowledgements[1].batch).toBe(0xffffffffffffffffn);
  const budget = f.channel.budget;
  for (const patch of [
    { ...patches(1)[0], cacheSlot: 1 },
    { destination: { width: 128, height: 64 }, cacheSlot: 0 },
    { destination: { width: 128, height: 64 }, cacheSlot: 1 },
  ]) {
    f.refinement.beginGeneration(1);
    const reservation = budget.reserveMessage(100);
    if (!reservation) throw new Error("Missing reservation");
    expect(() =>
      f.refinement.accept(
        { generation: 1, frame: 1, batch: 1n, kind: 1, receivedAt: 100 },
        [patch],
        256,
        128,
        reservation,
      ),
    ).toThrow();
    budget.finishMessage(reservation);
  }
  expect(f.counts().started).toBe(2);
});

test("queued supersession shares admission gates and releases its reservation bytes exactly once", () => {
  const f = fixture();
  const admission = f.refinement.beginGeneration(1);
  const binding: DesktopRefinementBinding = {
    generation: 1,
    frame: 1,
    batch: 1n,
    kind: 1,
    receivedAt: 100,
  };
  const reservation = f.channel.budget.reserveMessage(1024);
  if (!reservation) throw new Error("Missing reservation");
  f.refinement.supersede(
    binding,
    patches(1),
    4096,
    128,
    1,
    admission,
    reservation,
  );
  expect(f.counts().started).toBe(0);
  expect(f.acknowledgements).toHaveLength(1);
  expect(f.acknowledgements[0]).toMatchObject({
    generation: 1,
    frame: 1,
    batch: 1n,
    applied: false,
    charged: 0,
    active: 0,
  });
  expect(reservation.remainingBytes).toBe(0);
  expect(f.channel.budget.queuedMessageBytes).toBe(0);
  expect(f.channel.budget.queuedMessageCount).toBe(1);
  expect(() =>
    f.refinement.supersede(
      binding,
      patches(1),
      4096,
      128,
      1,
      admission,
      reservation,
    ),
  ).toThrow();
  expect(f.acknowledgements).toHaveLength(1);
  f.channel.budget.finishMessage(reservation);
  expect(reservation.active).toBe(false);
  expect(f.channel.budget.queuedMessageCount).toBe(0);
  expect(f.channel.budget.queuedMessageBytes).toBe(0);
  expect(f.channel.budget.auxiliaryBytes).toBe(0);
});

test("dynamic expiry starts at onmessage and background/identity changes cannot submit", async () => {
  for (const reason of [
    "elapsed",
    "background",
    "identity",
    "new-region",
  ] as const) {
    const f = fixture();
    f.enqueue(1n, 8, 2);
    await f.channel.whenIdle();
    if (reason === "elapsed") f.setClock(150);
    if (reason === "background") f.state.background = true;
    if (reason === "identity") f.state.identity = {};
    if (reason === "new-region") f.state.frame = f.state.displayedFrame = 2;
    f.refinement.baselineChanged();
    f.release();
    await until(() => f.acknowledgements.length === 1);
    expect(f.counts().draws).toBe(0);
    expect(f.counts().started).toBe(4);
    expect(f.counts().closed).toBe(4);
    expect(f.acknowledgements[0].applied).toBe(false);
  }
  const queued = fixture();
  queued.setClock(151);
  queued.enqueue(1n, 8, 2, 1024, 100);
  await queued.channel.whenIdle();
  expect(queued.counts().started).toBe(0);
  expect(queued.acknowledgements[0].applied).toBe(false);
});

test("baseline waits are local durations; output wakes them, retirement releases them without new decodes", async () => {
  for (const retire of [false, true]) {
    const f = fixture();
    f.state.displayedFrame = 0;
    f.enqueue(1n, 1);
    await f.channel.whenIdle();
    f.release();
    await until(() => f.counts().active === 0);
    expect(f.acknowledgements).toHaveLength(0);
    f.setClock(120);
    if (retire) f.refinement.retire();
    else {
      f.state.displayedFrame = 1;
      f.refinement.baselineChanged();
    }
    await until(() => f.acknowledgements.length === 1);
    expect(f.acknowledgements[0].applied).toBe(!retire);
    if (!retire) expect(f.timings[0].baselineWaitMicroseconds).toBe(20000);
    expect(f.counts().closed).toBe(1);
  }
});

test("failed native decode waits for late workers, cleans once, and ACKs before reporting failure", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const state: DesktopRefinementState = {
    generation: 1,
    frame: 1,
    displayedFrame: 1,
    identity: {},
    unavailable: false,
    background: false,
  };
  let closes = 0,
    calls = 0,
    draws = 0;
  const releases: (() => void)[] = [];
  const events: string[] = [];
  const tiles = new DesktopTiles(async () => {
    const index = calls++;
    await new Promise<void>((resolve) => releases.push(resolve));
    if (index === 0) throw new Error("PNG failed");
    return {
      width: 128,
      height: 64,
      close() {
        closes++;
      },
    };
  });
  const refinements = new DesktopRefinements(
    tiles,
    () => state,
    () => {
      draws++;
    },
    (_binding, applied) => {
      expect(applied).toBe(false);
      expect(channel.budget.auxiliaryBytes).toBe(0);
      expect(closes).toBe(3);
      events.push("ack");
    },
    () => {},
    () => events.push("failure"),
  );
  refinements.beginGeneration(1);
  channel.enqueue(
    new ArrayBuffer(1024),
    async (reservation) =>
      refinements.accept(
        {
          generation: 1,
          frame: 1,
          batch: 1n,
          kind: 1,
          receivedAt: performance.now(),
        },
        patches(4),
        512,
        64,
        reservation,
      ),
    () => events.push("receive-failure"),
  );
  await channel.whenIdle();
  releases[0]();
  await new Promise((resolve) => setTimeout(resolve, 0));
  expect(events).toEqual([]);
  expect(channel.budget.auxiliaryBytes).toBe(1024);
  for (const release of releases.slice(1)) release();
  await until(() => events.length === 2);
  expect(events).toEqual(["ack", "failure"]);
  expect(draws).toBe(0);
});

test("v2 validates aligned unique grid tiles and cumulative per-baseline limits before decoding", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const state: DesktopRefinementState = {
    generation: 1,
    frame: 1,
    displayedFrame: 1,
    identity: {},
    unavailable: false,
    background: false,
  };
  let decodes = 0,
    acknowledgements = 0;
  const tiles = new DesktopTiles(async () => {
    decodes++;
    return { width: 128, height: 64, close() {} };
  });
  const refinements = new DesktopRefinements(
    tiles,
    () => state,
    () => {},
    () => {
      acknowledgements++;
    },
    () => {},
    (error) => {
      throw error;
    },
  );
  refinements.beginGeneration(1);
  const accept = (batch: bigint, patchList: ultraterimal.wire.IPatchRect[]) => {
    const reservation = channel.budget.reserveMessage(256 * 1024);
    if (!reservation) throw new Error("Missing reservation");
    try {
      refinements.accept(
        {
          generation: 1,
          frame: 1,
          batch,
          kind: 1,
          receivedAt: performance.now(),
        },
        patchList,
        8192,
        2048,
        reservation,
      );
    } finally {
      channel.budget.finishMessage(reservation);
    }
  };
  const large = (index: number) => {
    const bytes = new Uint8Array(65536);
    bytes.set(png());
    return {
      destination: {
        x: (index % 64) * 128,
        y: Math.floor(index / 64) * 64,
        width: 128,
        height: 64,
      },
      png: bytes,
    };
  };
  expect(() => accept(1n, [large(0), large(0)])).toThrow();
  expect(() =>
    accept(1n, [
      { ...large(0), destination: { x: 1, width: 128, height: 64 } },
    ]),
  ).toThrow();
  expect(decodes).toBe(0);
  for (let index = 0; index < 32; index++) {
    accept(
      BigInt(index + 1),
      Array.from({ length: 4 }, (_, offset) => large(index * 4 + offset)),
    );
    await until(() => acknowledgements === index + 1);
  }
  expect(decodes).toBe(128);
  expect(() => accept(33n, [large(128)])).toThrow("baseline budget");
  expect(() => accept(33n, [large(0)])).toThrow("duplicate");
  expect(decodes).toBe(128);
});

test("legacy tiles still serialize receive and retain reliable remote cache transactions", async () => {
  const f = fixture();
  let nextReceived = false;
  f.channel.enqueue(
    new ArrayBuffer(100),
    async () => {
      const batch = await f.tiles.prepare(
        [{ ...patches(1)[0], cacheSlot: 1 }],
        256,
        128,
      );
      batch.commit();
      batch.dispose();
    },
    (error) => f.failures.push(error),
  );
  f.channel.enqueue(
    new ArrayBuffer(100),
    async () => {
      nextReceived = true;
      const batch = await f.tiles.prepare(
        [{ destination: { width: 128, height: 64 }, cacheSlot: 1 }],
        256,
        128,
      );
      batch.commit();
      batch.dispose();
    },
    (error) => f.failures.push(error),
  );
  await until(() => f.counts().started === 1);
  expect(nextReceived).toBe(false);
  f.release();
  await f.channel.whenIdle();
  expect(nextReceived).toBe(true);
  expect(f.counts().started).toBe(1);
  expect(f.counts().closed).toBe(0);
  f.tiles.clear();
  expect(f.counts().closed).toBe(1);
  expect(f.failures).toEqual([]);
});
