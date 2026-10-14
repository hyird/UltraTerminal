import { expect, test } from "bun:test";
import { ultraterimal } from "../../web/src/generated/session";
import { DesktopSession } from "../../web/src/sessions/desktop/desktop.session";
import type { DesktopPerformance } from "../../web/src/sessions/desktop/desktop.types";

const wire = ultraterimal.wire;
async function until(predicate: () => boolean): Promise<void> {
  for (let i = 0; i < 100 && !predicate(); i++)
    await new Promise((resolve) => setTimeout(resolve, 0));
  expect(predicate()).toBe(true);
}
function png() {
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
function harness(
  options: {
    fastBaselineTimeout?: boolean;
    manualPerformance?: boolean;
    pixelRatio?: number;
  } = {},
) {
  const globals = new Map<string, PropertyDescriptor | undefined>();
  const replace = (name: string, value: unknown) => {
    globals.set(name, Object.getOwnPropertyDescriptor(globalThis, name));
    Object.defineProperty(globalThis, name, {
      value,
      configurable: true,
      writable: true,
    });
  };
  const sockets: Socket[] = [];
  const ackResources: { active: number; closed: number }[] = [];
  class Socket {
    static OPEN = 1;
    readyState = 1;
    bufferedAmount = 0;
    binaryType = "arraybuffer";
    onopen: (() => void) | null = null;
    onmessage: ((event: { data: ArrayBuffer }) => void) | null = null;
    onerror: (() => void) | null = null;
    onclose: ((event: { code: number }) => void) | null = null;
    sent: ultraterimal.wire.Envelope[] = [];
    sequence = 0;
    constructor(readonly url: URL) {
      sockets.push(this);
    }
    send(bytes: Uint8Array) {
      const message = wire.Envelope.decode(bytes);
      if (message.desktopRefinementAck)
        ackResources.push({ active: pngActive, closed: pngClosed });
      this.sent.push(message);
    }
    close() {
      this.readyState = 3;
    }
    push(body: ultraterimal.wire.IEnvelope) {
      const channelId = this.url.pathname.endsWith("control")
        ? "control"
        : "video";
      const bytes = wire.Envelope.encode({
        protocolVersion: 1,
        sessionId: "session",
        channelId,
        sequence: ++this.sequence,
        ...body,
      }).finish();
      this.onmessage?.({ data: new Uint8Array(bytes).buffer });
    }
  }
  const decoders: Decoder[] = [];
  class Decoder {
    static async isConfigSupported(config: VideoDecoderConfig) {
      return { supported: true, config };
    }
    state = "unconfigured";
    chunks: EncodedVideoChunk[] = [];
    constructor(private readonly callbacks: VideoDecoderInit) {
      decoders.push(this);
    }
    configure() {
      this.state = "configured";
    }
    error() {
      this.callbacks.error(new DOMException("Unsupported codec"));
    }
    close() {
      this.state = "closed";
    }
    decode(chunk: EncodedVideoChunk) {
      this.chunks.push(chunk);
    }
    output(index = this.chunks.length - 1) {
      const timestamp = this.chunks[index].timestamp;
      this.callbacks.output({
        timestamp,
        displayWidth: 256,
        displayHeight: 128,
        close() {},
        marker: "video",
      } as unknown as VideoFrame);
    }
  }
  const draws: string[] = [];
  const context = {
    drawImage(image: { marker?: string }) {
      draws.push(image.marker ?? "png");
    },
    clearRect() {},
    putImageData() {},
  };
  const viewport = { clientWidth: 800, clientHeight: 600 };
  const stage = { parentElement: viewport, style: { width: "", height: "" } };
  const makeCanvas = () => ({
    width: 256,
    height: 128,
    dataset: {} as Record<string, string>,
    parentElement: stage,
    getContext: () => context,
    closest: () => null,
    style: { removeProperty() {}, setProperty() {} },
  });
  const canvas = makeCanvas();
  const overlay = makeCanvas();
  let visibilityChanged: (() => void) | undefined;
  const documentMock = {
    hidden: false,
    addEventListener(name: string, listener: () => void) {
      if (name === "visibilitychange") visibilityChanged = listener;
    },
  };
  let clock = 0;
  const publications: (() => void)[] = [];
  if (options.manualPerformance) replace("performance", { now: () => clock });
  if (options.fastBaselineTimeout || options.manualPerformance) {
    const nativeTimeout = globalThis.setTimeout;
    replace("setTimeout", (callback: () => void, delay?: number) => {
      if (options.manualPerformance && delay === 1000) {
        publications.push(callback);
        return 0;
      }
      return nativeTimeout(
        callback,
        options.fastBaselineTimeout && delay === 5000 ? 1 : delay,
      );
    });
  }
  let pngActive = 0,
    pngStarted = 0,
    pngClosed = 0;
  const pngReleases: (() => void)[] = [];
  replace("WebSocket", Socket);
  replace("VideoDecoder", Decoder);
  replace(
    "EncodedVideoChunk",
    class {
      timestamp: number;
      constructor(init: EncodedVideoChunkInit) {
        this.timestamp = init.timestamp;
      }
    },
  );
  replace(
    "ResizeObserver",
    class {
      observe() {}
      disconnect() {}
    },
  );
  replace("location", { href: "https://desktop.test/" });
  replace("document", documentMock);
  replace("window", {
    devicePixelRatio: options.pixelRatio ?? 1,
    matchMedia: () => ({ addEventListener() {} }),
  });
  replace("requestAnimationFrame", () => 1);
  replace("cancelAnimationFrame", () => {});
  replace("createImageBitmap", async () => {
    pngActive++;
    pngStarted++;
    await new Promise<void>((resolve) => pngReleases.push(resolve));
    pngActive--;
    return {
      width: 128,
      height: 64,
      close() {
        pngClosed++;
      },
    };
  });
  const endings: boolean[] = [];
  const samples: DesktopPerformance[] = [];
  const bitrates: number[] = [];
  const session = new DesktopSession(
    {
      id: "session",
      writeControl: false,
      channels: [
        { id: "control", purpose: 2, ticket: "control-ticket" },
        { id: "video", purpose: 3, ticket: "video-ticket" },
      ],
    },
    canvas as unknown as HTMLCanvasElement,
    overlay as unknown as HTMLCanvasElement,
    () => {},
    (_hadFrame, retryable) => endings.push(retryable),
    () => {},
    (bitrate) => bitrates.push(bitrate),
    (sample) => samples.push(sample),
  );
  const control = sockets[0],
    video = sockets[1];
  control.onopen?.();
  video.onopen?.();
  const config = (
    generation: number,
    contract = 2,
    codec = "avc1.42E033",
    width = 256,
    height = 128,
    continuousFrames = false,
  ) =>
    video.push({
      generation,
      videoConfig: {
        codec,
        width,
        height,
        desktopOptimization: true,
        desktopOptimizationContract: contract,
        continuousFrames,
      },
    });
  const frame = (generation: number, frameId: number) =>
    video.push({
      generation,
      frameId,
      videoFrame: {
        timestampMicroseconds: frameId,
        keyFrame: true,
        data: new Uint8Array([1, 2, 3]),
      },
    });
  const refine = (
    generation: number,
    frameId: number,
    batchId: NonNullable<ultraterimal.wire.IVideoRefinement["batchId"]> = 1,
    kind = 1,
    cacheSlot = 0,
  ) =>
    video.push({
      generation,
      frameId,
      baseFrameId: frameId,
      videoRefinement: {
        batchId,
        kind,
        patches: [
          { destination: { width: 128, height: 64 }, png: png(), cacheSlot },
        ],
      },
    });
  const release = () => {
    for (const resolve of pngReleases.splice(0)) resolve();
  };
  const ready = async (
    controlContract = 2,
    videoContract = 2,
    agentContract = 2,
    continuousFrames = false,
  ) => {
    control.push({
      channelReady: {
        purpose: 2,
        desktopOptimizationContract: controlContract,
        continuousFrames,
      },
    });
    video.push({
      channelReady: {
        purpose: 3,
        desktopOptimizationContract: videoContract,
        continuousFrames,
      },
    });
    config(1, agentContract, "avc1.42E033", 256, 128, continuousFrames);
    await until(() => decoders.length === 1);
    frame(1, 1);
    await until(() => decoders[0].chunks.length === 1);
    decoders[0].output();
  };
  const activate = async (continuousFrames = false) => {
    await ready(2, 2, 2, continuousFrames);
    config(2, 2, "avc1.42E033", 256, 128, continuousFrames);
    await until(() => decoders.length === 2);
    frame(2, 1);
    await until(() => decoders[1].chunks.length === 1);
    decoders[1].output();
  };
  return {
    session,
    stage,
    viewport,
    overlay,
    control,
    video,
    ackResources,
    config,
    frame,
    refine,
    ready,
    activate,
    release,
    draws,
    decoders,
    endings,
    samples,
    bitrates,
    advanceClock(milliseconds: number) {
      clock += milliseconds;
    },
    publishPerformance() {
      const publish = publications.shift();
      if (!publish) throw new Error("No pending performance publication");
      publish();
    },
    documentMock,
    hideDocument() {
      documentMock.hidden = true;
      visibilityChanged?.();
    },
    counts: () => ({
      active: pngActive,
      started: pngStarted,
      closed: pngClosed,
    }),
    async dispose() {
      session.stop();
      release();
      await until(() => pngActive === 0);
      await new Promise((resolve) => setTimeout(resolve, 0));
      for (const [name, descriptor] of globals) {
        if (descriptor) Object.defineProperty(globalThis, name, descriptor);
        else Reflect.deleteProperty(globalThis, name);
      }
    },
  };
}

test("DesktopSession fit preserves aspect ratio in both viewport orientations and native uses physical pixels", async () => {
  const h = harness({ pixelRatio: 2 });
  try {
    await h.ready();
    h.session.setScaleMode("fit");
    expect(h.stage.style).toEqual({ width: "800px", height: "400px" });
    h.viewport.clientWidth = 300;
    h.viewport.clientHeight = 900;
    h.session.setScaleMode("fit");
    expect(h.stage.style).toEqual({ width: "300px", height: "150px" });
    h.overlay.width = 1080;
    h.overlay.height = 1920;
    h.viewport.clientWidth = 800;
    h.viewport.clientHeight = 600;
    h.session.setScaleMode("fit");
    expect(h.stage.style).toEqual({ width: "337.5px", height: "600px" });
    h.session.setScaleMode("native");
    expect(h.stage.style).toEqual({ width: "540px", height: "960px" });
  } finally {
    await h.dispose();
  }
});

test("DesktopSession decoded repeats preserve sharp content and its outstanding refinement", async () => {
  const h = harness();
  try {
    await h.activate(true);
    expect(
      h.control.sent.some((m) => m.desktopVideoCapabilities?.continuousFrames),
    ).toBe(true);
    h.refine(2, 1);
    await until(() => h.counts().active === 1);
    for (const frameId of [2, 3]) {
      h.video.push({
        generation: 2,
        frameId,
        baseFrameId: 1,
        videoFrame: {
          timestampMicroseconds: frameId,
          repeated: true,
          data: new Uint8Array([4, 5]),
        },
      });
      await until(() => h.decoders[1].chunks.length === frameId);
      h.decoders[1].output();
    }
    expect(h.draws).toEqual(["video", "video"]);
    expect(h.control.sent.filter((m) => m.desktopRefinementAck)).toHaveLength(
      0,
    );
    h.release();
    await until(() => h.counts().closed === 1);
    expect(h.draws).toEqual(["video", "video", "png"]);
    expect(
      h.control.sent.find((m) => m.desktopRefinementAck)?.frameId.toString(),
    ).toBe("1");
    expect(
      h.control.sent.find((m) => m.desktopRefinementAck)?.desktopRefinementAck
        ?.status,
    ).toBe(wire.DesktopRefinementAck.Status.APPLIED);
    expect(
      h.control.sent
        .filter((m) => m.qualityFeedback)
        .at(-1)
        ?.qualityFeedback?.displayedFrame.toString(),
    ).toBe("3");
    h.frame(2, 4);
    await until(() => h.decoders[1].chunks.length === 4);
    h.decoders[1].output();
    expect(h.draws.at(-1)).toBe("video");
    expect(h.endings).toHaveLength(0);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession v2 refinement does not block new video and emits an independent post-cleanup superseded ACK", async () => {
  const h = harness();
  try {
    await h.activate();
    const capabilities = h.control.sent.filter(
      (message) => message.desktopVideoCapabilities,
    );
    expect(
      capabilities.some(
        (message) =>
          message.desktopVideoCapabilities?.desktopOptimizationContract === 2,
      ),
    ).toBe(true);
    h.refine(2, 1);
    await until(() => h.counts().active === 1);
    h.frame(2, 2);
    await until(() => h.decoders[1].chunks.length === 2);
    h.decoders[1].output();
    expect(h.counts().active).toBe(1);
    expect(h.draws).toEqual(["video", "video", "video"]);
    expect(
      h.control.sent.filter((message) => message.desktopRefinementAck),
    ).toHaveLength(0);
    h.release();
    await until(() =>
      h.control.sent.some((message) => message.desktopRefinementAck),
    );
    const ack = h.control.sent.find((message) => message.desktopRefinementAck);
    expect(ack?.generation.toString()).toBe("2");
    expect(ack?.frameId.toString()).toBe("1");
    expect(ack?.baseFrameId.toString()).toBe("0");
    expect(ack?.body).toBe("desktopRefinementAck");
    expect(ack?.desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.SUPERSEDED,
    );
    expect(ack?.qualityFeedback).toBeNull();
    expect(h.ackResources).toEqual([{ active: 0, closed: 1 }]);
    expect(h.counts().closed).toBe(1);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession a late static patch cannot restore old content after repeats and a reliable region", async () => {
  const h = harness();
  try {
    await h.activate(true);
    h.refine(2, 1);
    await until(() => h.counts().active === 1);
    h.video.push({
      generation: 2,
      frameId: 2,
      baseFrameId: 1,
      videoFrame: {
        timestampMicroseconds: 2,
        repeated: true,
        data: new Uint8Array([4, 5]),
      },
    });
    await until(() => h.decoders[1].chunks.length === 2);
    h.decoders[1].output();
    h.video.push({
      generation: 2,
      frameId: 3,
      baseFrameId: 2,
      desktopRegionUpdate: {
        patches: [
          { destination: { x: 128, width: 128, height: 64 }, png: png() },
        ],
      },
    });
    await until(() => h.counts().active === 2);
    h.release();
    await until(() => h.counts().closed === 2);
    expect(h.draws).toEqual(["video", "video", "png"]); // Only the reliable new content is drawn.
    expect(
      h.control.sent.find((m) => m.desktopRefinementAck)?.desktopRefinementAck
        ?.status,
    ).toBe(wire.DesktopRefinementAck.Status.SUPERSEDED);
    expect(
      h.control.sent
        .filter((m) => m.qualityFeedback)
        .at(-1)
        ?.qualityFeedback?.displayedFrame.toString(),
    ).toBe("3");
    expect(h.endings).toHaveLength(0);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession applied ACK and duration presence preserve the old video decode metric", async () => {
  const h = harness();
  try {
    await h.activate();
    const videoFeedback = h.control.sent
      .filter((message) => message.qualityFeedback)
      .at(-1)?.qualityFeedback;
    expect(videoFeedback?.displayedGeneration.toString()).toBe("2");
    expect(videoFeedback?.browserTiming).not.toBeNull();
    for (const field of [
      "receiveQueueMicroseconds",
      "decodeMicroseconds",
      "canvasSubmitMicroseconds",
    ])
      expect(Object.hasOwn(videoFeedback?.browserTiming ?? {}, field)).toBe(
        true,
      );
    expect(
      Object.hasOwn(
        videoFeedback?.browserTiming ?? {},
        "pngDecodeMicroseconds",
      ),
    ).toBe(false);
    expect(videoFeedback?.decodeMicroseconds).toBeGreaterThanOrEqual(0);
    h.refine(2, 1);
    await until(() => h.counts().active === 1);
    h.release();
    await until(() =>
      h.control.sent.some((message) => message.desktopRefinementAck),
    );
    expect(
      h.control.sent.find((message) => message.desktopRefinementAck)
        ?.desktopRefinementAck?.status,
    ).toBe(wire.DesktopRefinementAck.Status.APPLIED);
    const feedback = h.control.sent
      .filter((message) => message.qualityFeedback)
      .at(-1)?.qualityFeedback;
    expect(
      Object.hasOwn(feedback?.browserTiming ?? {}, "pngDecodeMicroseconds"),
    ).toBe(true);
    expect(
      Object.hasOwn(feedback?.browserTiming ?? {}, "baselineWaitMicroseconds"),
    ).toBe(false);
    expect(
      Object.hasOwn(feedback?.browserTiming ?? {}, "decodeMicroseconds"),
    ).toBe(false);
    expect(h.counts().closed).toBe(1);
    expect(h.draws.at(-1)).toBe("png");
    expect(h.ackResources).toEqual([{ active: 0, closed: 1 }]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession generation barrier and close do not await or commit late native PNG", async () => {
  for (const close of [false, true]) {
    const h = harness();
    try {
      await h.activate();
      h.refine(2, 1);
      await until(() => h.counts().active === 1);
      if (close) h.session.stop();
      else {
        h.config(3);
        await until(() => h.decoders.length === 3);
        h.frame(3, 1);
        await until(() => h.decoders[2].chunks.length === 1);
        h.decoders[2].output();
        h.refine(3, 1);
        await until(() =>
          h.control.sent.some(
            (message) =>
              message.generation.toString() === "3" &&
              message.desktopRefinementAck,
          ),
        );
        expect(h.counts().started).toBe(1);
        expect(
          h.control.sent.some(
            (message) =>
              message.generation.toString() === "2" &&
              message.desktopRefinementAck,
          ),
        ).toBe(false);
        expect(h.counts().active).toBe(1);
        expect(h.counts().closed).toBe(0);
      }
      h.release();
      await until(() => h.counts().closed === 1);
      expect(h.draws.includes("png")).toBe(false);
      if (!close)
        await until(() =>
          h.control.sent.some(
            (message) =>
              message.generation.toString() === "2" &&
              message.desktopRefinementAck,
          ),
        );
      expect(h.endings).toEqual([]);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession legacy/unknown gates keep refinement serialized and remote cache definitions supported", async () => {
  for (const [controlContract, videoContract, agentContract] of [
    [0, 2, 2],
    [3, 2, 2],
    [2, 0, 2],
    [2, 3, 2],
    [2, 2, 0],
    [2, 2, 3],
  ]) {
    const h = harness();
    try {
      await h.ready(controlContract, videoContract, agentContract);
      h.config(2, agentContract);
      await until(() => h.decoders.length === 2);
      h.frame(2, 1);
      await until(() => h.decoders[1].chunks.length === 1);
      h.decoders[1].output();
      h.refine(2, 1, 0, 0, 1);
      await until(() => h.counts().active === 1);
      h.frame(2, 2);
      await new Promise((resolve) => setTimeout(resolve, 0));
      expect(h.decoders[1].chunks).toHaveLength(1);
      h.release();
      await until(() => h.decoders[1].chunks.length === 2);
      expect(h.draws.at(-1)).toBe("png");
      expect(h.counts().closed).toBe(0);
      expect(
        h.control.sent.some((message) => message.desktopRefinementAck),
      ).toBe(false);
      expect(
        h.control.sent.some(
          (message) =>
            message.desktopVideoCapabilities?.desktopOptimizationContract === 2,
        ),
      ).toBe(false);
      expect(h.endings).toEqual([]);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession reliable region update remains FIFO even with v2 active", async () => {
  const h = harness();
  try {
    await h.activate();
    h.video.push({
      generation: 2,
      frameId: 2,
      baseFrameId: 1,
      desktopRegionUpdate: {
        patches: [
          { destination: { width: 128, height: 64 }, png: png(), cacheSlot: 1 },
        ],
      },
    });
    await until(() => h.counts().active === 1);
    h.frame(2, 3);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(h.decoders[1].chunks).toHaveLength(1);
    h.release();
    await until(() => h.decoders[1].chunks.length === 2);
    expect(h.draws.at(-1)).toBe("png");
    expect(h.counts().closed).toBe(0);
    expect(h.control.sent.some((message) => message.desktopRefinementAck)).toBe(
      false,
    );
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession dynamic refinement retires in hidden document or hidden view, then closes late bitmaps", async () => {
  for (const hideDocument of [false, true]) {
    const h = harness();
    try {
      await h.activate();
      h.refine(2, 1, 1, wire.RefinementKind.DYNAMIC_DETAIL);
      await until(() => h.counts().active === 1);
      if (hideDocument) h.hideDocument();
      else h.session.setVisible(false);
      expect(
        h.control.sent.some((message) => message.desktopRefinementAck),
      ).toBe(false);
      expect(h.counts().active).toBe(1);
      h.release();
      await until(() =>
        h.control.sent.some((message) => message.desktopRefinementAck),
      );
      expect(h.counts().closed).toBe(1);
      expect(h.draws.includes("png")).toBe(false);
      expect(
        h.control.sent.find((message) => message.desktopRefinementAck)
          ?.desktopRefinementAck?.status,
      ).toBe(wire.DesktopRefinementAck.Status.SUPERSEDED);
      h.frame(2, 2);
      await until(() => h.decoders[1].chunks.length === 2);
      h.decoders[1].output();
      expect(h.endings).toEqual([]);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession optional baseline timeout only supersedes refinement, even before normal video ACK", async () => {
  const h = harness({ fastBaselineTimeout: true });
  try {
    await h.activate();
    h.frame(2, 2);
    await until(() => h.decoders[1].chunks.length === 2);
    h.refine(2, 2);
    await until(() => h.counts().active === 1);
    h.release();
    await until(() =>
      h.control.sent.some((message) => message.desktopRefinementAck),
    );
    const ack = h.control.sent.find((message) => message.desktopRefinementAck);
    expect(ack?.frameId.toString()).toBe("2");
    expect(ack?.desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.SUPERSEDED,
    );
    expect(h.counts().closed).toBe(1);
    expect(h.draws.includes("png")).toBe(false);
    expect(h.endings).toEqual([]);
    h.decoders[1].output();
    h.frame(2, 3);
    await until(() => h.decoders[1].chunks.length === 3);
    h.decoders[1].output();
    expect(h.endings).toEqual([]);
    expect(
      h.control.sent
        .filter((message) => message.qualityFeedback)
        .at(-1)
        ?.qualityFeedback?.displayedFrame.toString(),
    ).toBe("3");
  } finally {
    await h.dispose();
  }
});

test("DesktopSession an actual baseline wait is present and submitted only after matching video output", async () => {
  const h = harness();
  try {
    await h.activate();
    h.frame(2, 2);
    await until(() => h.decoders[1].chunks.length === 2);
    h.refine(2, 2);
    await until(() => h.counts().active === 1);
    h.release();
    await until(() => h.counts().active === 0);
    expect(h.control.sent.some((message) => message.desktopRefinementAck)).toBe(
      false,
    );
    expect(h.counts().closed).toBe(0);
    h.decoders[1].output();
    await until(() =>
      h.control.sent.some((message) => message.desktopRefinementAck),
    );
    const feedback = h.control.sent
      .filter((message) => message.qualityFeedback)
      .at(-1)?.qualityFeedback;
    expect(
      Object.hasOwn(feedback?.browserTiming ?? {}, "baselineWaitMicroseconds"),
    ).toBe(true);
    expect(
      feedback?.browserTiming?.baselineWaitMicroseconds,
    ).toBeGreaterThanOrEqual(0);
    expect(
      h.control.sent.find((message) => message.desktopRefinementAck)
        ?.desktopRefinementAck?.status,
    ).toBe(wire.DesktopRefinementAck.Status.APPLIED);
    expect(h.counts().closed).toBe(1);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession reference-only reliable region preserves absent PNG and baseline-wait stages on the wire", async () => {
  const h = harness();
  try {
    await h.activate();
    h.video.push({
      generation: 2,
      frameId: 2,
      baseFrameId: 1,
      desktopRegionUpdate: {
        patches: [
          { destination: { width: 128, height: 64 }, png: png(), cacheSlot: 1 },
        ],
      },
    });
    await until(() => h.counts().active === 1);
    h.release();
    await until(() =>
      h.control.sent.some(
        (message) => message.qualityFeedback?.displayedFrame.toString() === "2",
      ),
    );
    h.video.push({
      generation: 2,
      frameId: 3,
      baseFrameId: 2,
      desktopRegionUpdate: {
        patches: [{ destination: { width: 128, height: 64 }, cacheSlot: 1 }],
      },
    });
    await until(() =>
      h.control.sent.some(
        (message) => message.qualityFeedback?.displayedFrame.toString() === "3",
      ),
    );
    const timing = h.control.sent
      .filter((message) => message.qualityFeedback)
      .at(-1)?.qualityFeedback?.browserTiming;
    expect(Object.hasOwn(timing ?? {}, "canvasSubmitMicroseconds")).toBe(true);
    for (const field of [
      "decodeMicroseconds",
      "pngDecodeMicroseconds",
      "baselineWaitMicroseconds",
      "receiveQueueMicroseconds",
    ])
      expect(Object.hasOwn(timing ?? {}, field)).toBe(false);
    expect(h.counts().started).toBe(1);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession sends full uint64 ACK batch IDs as independent oneof bodies with numeric status", async () => {
  const h = harness();
  try {
    await h.activate();
    const batch = wire.VideoRefinement.fromObject({
      batchId: "18446744073709551615",
    }).batchId;
    h.refine(2, 1, batch);
    await until(() => h.counts().active === 1);
    h.release();
    await until(() =>
      h.control.sent.some((message) => message.desktopRefinementAck),
    );
    const ack = h.control.sent.find((message) => message.desktopRefinementAck);
    expect(ack?.body).toBe("desktopRefinementAck");
    expect(ack?.desktopRefinementAck?.batchId.toString()).toBe(
      "18446744073709551615",
    );
    expect(ack?.desktopRefinementAck?.status).toBe(1);
    expect(ack?.qualityFeedback).toBeNull();
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
  const timing = wire.DesktopBrowserTiming.decode(
    wire.DesktopBrowserTiming.encode({ decodeMicroseconds: 0 }).finish(),
  );
  expect(Object.hasOwn(timing, "decodeMicroseconds")).toBe(true);
  expect(timing.decodeMicroseconds).toBe(0);
  expect(Object.hasOwn(timing, "pngDecodeMicroseconds")).toBe(false);
  expect(timing.pngDecodeMicroseconds).toBeNull();
});

async function fillDecodeSlots(h: ReturnType<typeof harness>) {
  const decoder = h.decoders.at(-1);
  if (!decoder) throw new Error("Missing decoder");
  for (let frame = 2; frame <= 5; frame++) h.frame(2, frame);
  await until(() => decoder.chunks.length === 5);
  h.frame(2, 6);
  // Let the real serialized receive chain reach its fifth slot acquisition.
  await new Promise((resolve) => setTimeout(resolve, 0));
  expect(decoder.chunks).toHaveLength(5);
}

test("DesktopSession sustained decode pressure preserves bounded admission and timing", async () => {
  const h = harness({ manualPerformance: true });
  try {
    await h.activate();
    const firstFeedback = h.control.sent.find((m) => m.qualityFeedback);
    expect(firstFeedback?.qualityFeedback).toBeDefined();
    await fillDecodeSlots(h);
    for (let cycle = 0; cycle < 4; cycle++) {
      h.advanceClock(200);
      h.decoders[1].output(cycle + 1);
      await until(() => h.decoders[1].chunks.length === cycle + 6);
      h.frame(2, cycle + 7);
      await new Promise((resolve) => setTimeout(resolve, 0));
      // A fifth pending image waits for a slot; it is not dropped or decoded.
      expect(h.decoders[1].chunks).toHaveLength(cycle + 6);
      h.advanceClock(1000);
      h.publishPerformance();
      const sample = h.samples.at(-1);
      expect(sample?.pendingFrames).toBe(4);
      expect(sample?.receivedQueueBytes).toBeGreaterThan(0);
      expect(sample?.decodeMilliseconds).toBeGreaterThan(50);
      expect(h.bitrates.at(-1)).toBeGreaterThan(0);
    }
    h.session.setVisible(false);
    expect(
      h.control.sent.filter((m) => m.qualityFeedback).at(-1)?.qualityFeedback
        ?.background,
    ).toBe(true);
    h.session.setVisible(true);
    h.config(3);
    await until(() => h.decoders.length === 3);
    h.frame(3, 1);
    await until(() => h.decoders[2].chunks.length === 1);
    h.decoders[2].output();
    const feedback = h.control.sent
      .filter((m) => m.qualityFeedback)
      .map((m) => m.qualityFeedback);
    expect(feedback.length).toBeGreaterThan(4);
    expect(feedback.some((m) => (m?.queueDepth ?? 0) >= 4)).toBe(true);
    expect(feedback.some((m) => (m?.decodeMicroseconds ?? 0) > 50000)).toBe(
      true,
    );
    expect(feedback.at(-1)?.displayedGeneration.toString()).toBe("3");
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

function refinementAcks(h: ReturnType<typeof harness>) {
  return h.control.sent.filter((message) => message.desktopRefinementAck);
}

test("DesktopSession slot-full queued refinement returns exact cleanup credit once before the next generation refines", async () => {
  const h = harness();
  try {
    await h.activate();
    await fillDecodeSlots(h);
    h.frame(2, 7);
    h.refine(2, 7, 37);
    h.config(3);
    await until(() => h.decoders.length === 3);
    expect(h.counts()).toEqual({ active: 0, started: 0, closed: 0 });
    const acks = refinementAcks(h);
    expect(acks).toHaveLength(1);
    expect(acks[0].generation.toString()).toBe("2");
    expect(acks[0].frameId.toString()).toBe("7");
    expect(acks[0].baseFrameId.toString()).toBe("0");
    expect(acks[0].desktopRefinementAck?.batchId.toString()).toBe("37");
    expect(acks[0].desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.SUPERSEDED,
    );
    h.frame(3, 1);
    await until(() => h.decoders[2].chunks.length === 1);
    h.decoders[2].output();
    h.refine(3, 1);
    await until(() => h.counts().started === 1);
    h.release();
    await until(() => refinementAcks(h).length === 2);
    expect(refinementAcks(h)[1].desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.APPLIED,
    );
    expect(h.ackResources).toEqual([
      { active: 0, closed: 0 },
      { active: 0, closed: 1 },
    ]);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession codec rejection unblocks slot-full queued cleanup without starting PNG", async () => {
  const h = harness();
  try {
    await h.ready();
    h.config(2, 2, "hvc1.1.6.L123.B0");
    await until(() => h.decoders.length === 2);
    h.frame(2, 1);
    await until(() => h.decoders[1].chunks.length === 1);
    h.decoders[1].output();
    await fillDecodeSlots(h);
    h.refine(2, 6, 41);
    h.decoders[1].error();
    await until(() => refinementAcks(h).length === 1);
    expect(h.counts().started).toBe(0);
    expect(refinementAcks(h)[0].desktopRefinementAck?.batchId.toString()).toBe(
      "41",
    );
    expect(refinementAcks(h)[0].desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.SUPERSEDED,
    );
    h.config(3);
    await until(() => h.decoders.length === 3);
    h.frame(3, 1);
    await until(() => h.decoders[2].chunks.length === 1);
    h.decoders[2].output();
    h.refine(3, 1);
    await until(() => h.counts().started === 1);
    h.release();
    await until(() => refinementAcks(h).length === 2);
    expect(refinementAcks(h)[1].desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.APPLIED,
    );
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession queued cleanup rejects illegal tuples and malformed v2 PNG before ACK or native decoding", async () => {
  const valid = {
    batchId: 7,
    kind: 1,
    patches: [{ destination: { width: 128, height: 64 }, png: png() }],
  };
  for (const body of [
    { frameId: 99, baseFrameId: 99 },
    { baseFrameId: 5 },
    { sequence: 1 },
    { sessionId: "foreign" },
    { channelId: "foreign" },
    { videoRefinement: { ...valid, batchId: 0 } },
    { videoRefinement: { ...valid, kind: 3 } },
    { videoRefinement: { ...valid, patches: [] } },
    {
      videoRefinement: {
        ...valid,
        patches: [{ ...valid.patches[0], cacheSlot: 1 }],
      },
    },
    {
      videoRefinement: {
        ...valid,
        patches: [{ ...valid.patches[0], png: new Uint8Array(33) }],
      },
    },
    {
      videoRefinement: {
        ...valid,
        patches: [
          {
            ...valid.patches[0],
            destination: { x: 1, width: 128, height: 64 },
          },
        ],
      },
    },
    {
      videoRefinement: {
        ...valid,
        patches: [valid.patches[0], valid.patches[0]],
      },
    },
  ]) {
    const h = harness();
    try {
      await h.activate();
      await fillDecodeSlots(h);
      h.video.push({
        generation: 2,
        frameId: 6,
        baseFrameId: 6,
        videoRefinement: valid,
        ...body,
      });
      h.config(3);
      await until(() => h.endings.length === 1);
      expect(refinementAcks(h)).toHaveLength(0);
      expect(h.counts().started).toBe(0);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession retained generation gates and geometry do not borrow the current v2 configuration", async () => {
  for (const oldContract of [0, 2, 3]) {
    const h = harness();
    try {
      await h.ready(2, 2, oldContract);
      h.config(2, oldContract);
      await until(() => h.decoders.length === 2);
      h.frame(2, 1);
      await until(() => h.decoders[1].chunks.length === 1);
      h.decoders[1].output();
      h.config(3, oldContract === 2 ? 0 : 2, "avc1.42E033", 128, 64);
      await until(() => h.decoders.length === 3);
      h.video.push({
        generation: 2,
        frameId: 1,
        baseFrameId: 1,
        videoRefinement: {
          batchId: 9,
          kind: 1,
          patches: [
            { destination: { x: 128, width: 128, height: 64 }, png: png() },
          ],
        },
      });
      h.frame(3, 1);
      await until(() => h.decoders[2].chunks.length === 1);
      expect(refinementAcks(h)).toHaveLength(oldContract === 2 ? 1 : 0);
      expect(h.counts().started).toBe(0);
      expect(h.endings).toEqual([]);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession unnegotiated and unknown queued generations never return v2 credit", async () => {
  const cases: [number, number, number][] = [
    [0, 2, 2],
    [2, 0, 2],
    [2, 2, 0],
    [2, 2, 3],
    [2, 2, 2],
  ];
  for (const contracts of cases) {
    const h = harness();
    try {
      await h.ready(...contracts);
      h.config(2, contracts[2]);
      await until(() => h.decoders.length === 2);
      h.frame(2, 1);
      await until(() => h.decoders[1].chunks.length === 1);
      h.decoders[1].output();
      await fillDecodeSlots(h);
      const unknown = contracts.every((contract) => contract === 2);
      h.refine(unknown ? 0 : 2, 6);
      h.config(3);
      await until(() =>
        unknown ? h.endings.length === 1 : h.decoders.length === 3,
      );
      expect(refinementAcks(h)).toHaveLength(0);
      expect(h.counts().started).toBe(0);
      expect(h.endings).toHaveLength(unknown ? 1 : 0);
    } finally {
      await h.dispose();
    }
  }
});

test("DesktopSession codec rejection of a started task still waits for late bitmap cleanup", async () => {
  const h = harness();
  try {
    await h.ready();
    h.config(2, 2, "hvc1.1.6.L123.B0");
    await until(() => h.decoders.length === 2);
    h.frame(2, 1);
    await until(() => h.decoders[1].chunks.length === 1);
    h.decoders[1].output();
    h.refine(2, 1, 77);
    await until(() => h.counts().active === 1);
    h.decoders[1].error();
    h.config(3);
    await until(() => h.decoders.length === 3);
    expect(refinementAcks(h)).toHaveLength(0);
    expect(h.counts().closed).toBe(0);
    h.release();
    await until(() => refinementAcks(h).length === 1);
    expect(h.ackResources).toEqual([{ active: 0, closed: 1 }]);
    expect(refinementAcks(h)[0].desktopRefinementAck?.batchId.toString()).toBe(
      "77",
    );
    expect(refinementAcks(h)[0].desktopRefinementAck?.status).toBe(
      wire.DesktopRefinementAck.Status.SUPERSEDED,
    );
    expect(h.draws.includes("png")).toBe(false);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession a reliable region baseline wait stays FIFO ahead of a queued configuration", async () => {
  const h = harness();
  try {
    await h.activate();
    h.frame(2, 2);
    await until(() => h.decoders[1].chunks.length === 2);
    h.video.push({
      generation: 2,
      frameId: 3,
      baseFrameId: 2,
      desktopRegionUpdate: {
        patches: [
          { destination: { width: 128, height: 64 }, png: png(), cacheSlot: 1 },
        ],
      },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    h.config(3);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(h.decoders).toHaveLength(2);
    h.decoders[1].output();
    await until(() => h.counts().started === 1);
    expect(h.decoders).toHaveLength(2);
    h.release();
    await until(() => h.decoders.length === 3);
    expect(h.draws.at(-1)).toBe("png");
    expect(refinementAcks(h)).toHaveLength(0);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession rejects replayed sequence and foreign bindings before decoding", async () => {
  for (const body of [
    { sequence: 1 },
    { sessionId: "foreign" },
    { channelId: "foreign" },
  ]) {
    const h = harness();
    try {
      await h.activate();
      h.video.push({
        ...body,
        generation: 2,
        frameId: 1,
        baseFrameId: 1,
        videoRefinement: {
          batchId: 1,
          kind: 1,
          patches: [{ destination: { width: 128, height: 64 }, png: png() }],
        },
      });
      await until(() => h.endings.length === 1);
      expect(h.counts().started).toBe(0);
    } finally {
      await h.dispose();
    }
  }
});
