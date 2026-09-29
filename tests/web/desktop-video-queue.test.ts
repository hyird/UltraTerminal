import { expect, test } from "bun:test";
import { ultraterimal } from "../../web/src/generated/session";
import {
  DesktopVideoDecodeLease,
  DesktopVideoDecodeTimeoutError,
  DesktopVideoReceiveChannel,
  maximumDesktopMessageBytes,
  maximumDesktopReceiveBytes,
  maximumPendingVideoFrames,
  maximumSubmittedDecodeFrames,
  videoDecodeSlotTimeoutMilliseconds,
} from "../../web/src/sessions/desktop/video-queue.session";

const feedback = ultraterimal.wire.QualityFeedback;

async function waitUntil(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt++) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  expect(predicate()).toBe(true);
}

function enqueueFrame(
  channel: DesktopVideoReceiveChannel,
  id: number,
  state: {
    submitted: number[];
    leases: Map<number, DesktopVideoDecodeLease>;
    stopped: boolean;
    failures: string[];
  },
  timeout = videoDecodeSlotTimeoutMilliseconds,
): boolean {
  const messageBytes = 1024;
  const encodedBytes = 512;
  return channel.enqueue(
    new ArrayBuffer(messageBytes),
    async (reservation) => {
      if (channel.isRetiredGeneration(1, 1)) return;
      const result = await channel.submitFrame(
        reservation,
        id,
        encodedBytes,
        timeout,
        () =>
          !state.stopped &&
          !channel.isReconfiguring &&
          !channel.isRetiredGeneration(1, 1),
        (lease) => {
          if (state.stopped) throw new Error("decode used after close");
          state.submitted.push(id);
          state.leases.set(id, lease);
        },
      );
      if (result === "cancelled" && !state.stopped && !channel.isReconfiguring)
        throw new Error("Unexpected test decode cancellation");
    },
    (error) => {
      state.failures.push(
        error instanceof DesktopVideoDecodeTimeoutError
          ? "timeout"
          : `frame-${id}`,
      );
      state.stopped = true;
      channel.close();
    },
  );
}

test("round-trips the new receive capacity and retains protobuf's zero default", () => {
  const oldClient = feedback.decode(feedback.encode({}).finish());
  const newClient = feedback.decode(
    feedback.encode({ maximumPendingVideoFrames }).finish(),
  );
  expect(oldClient.maximumPendingVideoFrames).toBe(0);
  expect(newClient.maximumPendingVideoFrames).toBe(128);
});

test("serial channel admits only 16 decode submissions during a 40-frame burst", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const state = {
    submitted: [] as number[],
    leases: new Map<number, DesktopVideoDecodeLease>(),
    stopped: false,
    failures: [] as string[],
  };
  for (let id = 1; id <= 40; id++)
    expect(enqueueFrame(channel, id, state)).toBe(true);

  await waitUntil(() => channel.decodeWaiterCount === 1);
  expect(state.submitted).toEqual(
    Array.from(
      { length: maximumSubmittedDecodeFrames },
      (_, index) => index + 1,
    ),
  );
  expect(channel.budget.pendingFrameCount).toBe(16);
  expect(channel.budget.pendingFrameBytes).toBe(16 * 512);
  expect(channel.budget.queuedMessageCount).toBe(24);
  expect(channel.budget.queuedMessageBytes).toBe(24 * 1024);
  expect(
    channel.budget.pendingFrameBytes + channel.budget.queuedMessageBytes,
  ).toBeLessThanOrEqual(maximumDesktopReceiveBytes);

  for (let next = 17; next <= 40; next++) {
    const displayed = next - maximumSubmittedDecodeFrames;
    expect(channel.finishFrame(displayed, state.leases.get(displayed)!)).toBe(
      true,
    );
    await waitUntil(() => state.submitted.length === next);
    expect(state.submitted.at(-1)).toBe(next);
    expect(channel.budget.pendingFrameCount).toBeLessThanOrEqual(16);
  }
  for (let id = 25; id <= 40; id++)
    expect(channel.finishFrame(id, state.leases.get(id)!)).toBe(true);
  await channel.whenIdle();
  expect(state.submitted).toEqual(
    Array.from({ length: 40 }, (_, index) => index + 1),
  );
  expect(channel.budget.pendingFrameCount).toBe(0);
  expect(channel.budget.queuedMessageCount).toBe(0);
  expect(state.failures).toEqual([]);
});

test("transfers the current message reservation without double-counting at the byte limit", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const submitted: number[] = [];
  const leases = new Map<number, DesktopVideoDecodeLease>();
  let closed = false;
  const enqueue = (id: number, messageBytes: number, encodedBytes: number) =>
    channel.enqueue(
      new ArrayBuffer(messageBytes),
      async (reservation) => {
        const result = await channel.submitFrame(
          reservation,
          id,
          encodedBytes,
          videoDecodeSlotTimeoutMilliseconds,
          () => !closed,
          (lease) => {
            submitted.push(id);
            leases.set(id, lease);
          },
        );
        if (result === "cancelled" && !closed)
          throw new Error("Unexpected handoff cancellation");
      },
      () => {
        closed = true;
        channel.close();
      },
    );

  for (let id = 1; id <= 16; id++)
    expect(enqueue(id, 1_000_000, 1_000_000)).toBe(true);
  expect(
    enqueue(17, maximumDesktopMessageBytes, maximumDesktopMessageBytes),
  ).toBe(true);
  expect(enqueue(18, 760_000, 512)).toBe(true);
  await waitUntil(() => channel.decodeWaiterCount === 1);
  expect(
    channel.budget.pendingFrameBytes + channel.budget.queuedMessageBytes,
  ).toBeLessThanOrEqual(maximumDesktopReceiveBytes);

  expect(channel.finishFrame(1, leases.get(1)!)).toBe(true);
  await waitUntil(() => submitted.length === 17);
  expect(channel.budget.pendingFrameBytes).toBe(
    15_000_000 + maximumDesktopMessageBytes,
  );
  expect(channel.budget.queuedMessageBytes).toBe(760_000);
  expect(
    channel.budget.pendingFrameBytes + channel.budget.queuedMessageBytes,
  ).toBeLessThanOrEqual(maximumDesktopReceiveBytes);

  closed = true;
  channel.close();
  await channel.whenIdle();
  expect(channel.budget.pendingFrameCount).toBe(0);
  expect(channel.budget.queuedMessageCount).toBe(0);
});

test("reconfiguration interrupts a waiter and drains retired frames before its config", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const state = {
    submitted: [] as number[],
    leases: new Map<number, DesktopVideoDecodeLease>(),
    stopped: false,
    failures: [] as string[],
  };
  for (let id = 1; id <= 40; id++)
    expect(enqueueFrame(channel, id, state)).toBe(true);
  await waitUntil(() => channel.decodeWaiterCount === 1);

  expect(
    channel.enqueue(
      new ArrayBuffer(64),
      async () => {
        channel.beginVideoGeneration(2);
        channel.completeVideoGeneration(2);
      },
      () => state.failures.push("configuration"),
      () => channel.announceVideoGeneration(2, 1),
    ),
  ).toBe(true);
  await channel.whenIdle();
  expect(state.submitted).toHaveLength(16);
  expect(channel.decodeWaiterCount).toBe(0);
  expect(channel.budget.pendingFrameCount).toBe(0);
  expect(channel.budget.queuedMessageCount).toBe(0);
  expect(state.failures).toEqual([]);
  channel.close();
});

test("stop and decoder failure close the gate and release a waiting receive", async () => {
  for (const reason of ["stop", "decoder-error"]) {
    const channel = new DesktopVideoReceiveChannel(true);
    const state = {
      submitted: [] as number[],
      leases: new Map<number, DesktopVideoDecodeLease>(),
      stopped: false,
      failures: [] as string[],
    };
    for (let id = 1; id <= 17; id++)
      expect(enqueueFrame(channel, id, state)).toBe(true);
    await waitUntil(() => channel.decodeWaiterCount === 1);
    if (reason === "decoder-error") state.failures.push(reason);
    state.stopped = true;
    channel.close();
    await channel.whenIdle();
    expect(state.submitted).toHaveLength(16);
    expect(channel.decodeWaiterCount).toBe(0);
    expect(channel.budget.pendingFrameCount).toBe(0);
    expect(channel.budget.queuedMessageCount).toBe(0);
    expect(
      channel.enqueue(
        new ArrayBuffer(1),
        async () => {},
        () => {},
      ),
    ).toBe(false);
  }
});

test("decode-slot timeout fails explicitly and releases channel accounting", async () => {
  const channel = new DesktopVideoReceiveChannel(true);
  const state = {
    submitted: [] as number[],
    leases: new Map<number, DesktopVideoDecodeLease>(),
    stopped: false,
    failures: [] as string[],
  };
  for (let id = 1; id <= 17; id++)
    expect(enqueueFrame(channel, id, state, 5)).toBe(true);
  await channel.whenIdle();
  expect(state.submitted).toHaveLength(16);
  expect(state.failures).toEqual(["timeout"]);
  expect(channel.decodeWaiterCount).toBe(0);
  expect(channel.budget.pendingFrameCount).toBe(0);
  expect(channel.budget.queuedMessageCount).toBe(0);
});

test("receive message count and byte limits remain hard failures", async () => {
  const countBound = new DesktopVideoReceiveChannel(true);
  for (let id = 0; id < 130; id++)
    expect(
      countBound.enqueue(
        new ArrayBuffer(0),
        async () => {},
        () => {},
      ),
    ).toBe(true);
  expect(
    countBound.enqueue(
      new ArrayBuffer(0),
      async () => {},
      () => {},
    ),
  ).toBe(false);
  await countBound.whenIdle();

  const byteBound = new DesktopVideoReceiveChannel(true);
  const halfWindow = maximumDesktopReceiveBytes / 2;
  expect(
    byteBound.enqueue(
      new ArrayBuffer(halfWindow),
      async () => {},
      () => {},
    ),
  ).toBe(true);
  expect(
    byteBound.enqueue(
      new ArrayBuffer(halfWindow),
      async () => {},
      () => {},
    ),
  ).toBe(true);
  expect(
    byteBound.enqueue(
      new ArrayBuffer(1),
      async () => {},
      () => {},
    ),
  ).toBe(false);
  await byteBound.whenIdle();

  expect(maximumDesktopMessageBytes).toBe(16 * 1024 * 1024);
  expect(byteBound.budget.queuedMessageCount).toBe(0);
});
