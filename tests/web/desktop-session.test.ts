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
function harness(
  options: {
    fastDecodeTimeout?: boolean;
    stalledSupportCodec?: string;
    deferredCodecDetection?: boolean;
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
  const codecDetectionReleases: (() => void)[] = [];
  class Decoder {
    static async isConfigSupported(config: VideoDecoderConfig) {
      if (options.deferredCodecDetection && config.codedWidth === 4096)
        await new Promise<void>((resolve) =>
          codecDetectionReleases.push(resolve),
        );
      if (config.codec === options.stalledSupportCodec)
        await new Promise<void>(() => {});
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
      draws.push(image.marker ?? "video");
    },
    clearRect() {},
    putImageData() {},
  };
  const viewport = { clientWidth: 800, clientHeight: 600 };
  const stage = { parentElement: viewport, style: { width: "", height: "" } };
  const makeCanvas = () => {
    let attributes: unknown;
    return {
      width: 256,
      height: 128,
      dataset: {} as Record<string, string>,
      parentElement: stage,
      get attributes() {
        return attributes;
      },
      getContext: (_type: string, options?: unknown) => {
        attributes = options;
        return context;
      },
      closest: () => null,
      style: { removeProperty() {}, setProperty() {} },
    };
  };
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
  if (options.fastDecodeTimeout || options.manualPerformance) {
    const nativeTimeout = globalThis.setTimeout;
    replace("setTimeout", (callback: () => void, delay?: number) => {
      if (options.manualPerformance && delay === 1000) {
        publications.push(callback);
        return 0;
      }
      return nativeTimeout(
        callback,
        options.fastDecodeTimeout && delay === 4000 ? 10 : delay,
      );
    });
  }
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
  const endings: boolean[] = [];
  const statuses: string[] = [];
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
    (status) => statuses.push(status),
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
    canvas,
    stage,
    viewport,
    overlay,
    control,
    video,
    config,
    frame,
    ready,
    activate,
    draws,
    decoders,
    endings,
    statuses,
    releaseCodecDetection() {
      for (const resolve of codecDetectionReleases.splice(0)) resolve();
    },
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
    async dispose() {
      session.stop();
      for (const resolve of codecDetectionReleases.splice(0)) resolve();
      await new Promise((resolve) => setTimeout(resolve, 0));
      for (const [name, descriptor] of globals) {
        if (descriptor) Object.defineProperty(globalThis, name, descriptor);
        else Reflect.deleteProperty(globalThis, name);
      }
    },
  };
}

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

test("DesktopSession only reports paired after both channels are ready", async () => {
  const h = harness();
  try {
    expect(h.canvas.attributes).toEqual({
      alpha: false,
      desynchronized: false,
    });
    h.control.push({ channelReady: { purpose: 2 } });
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(h.statuses.some((status) => status.includes("通道已配对"))).toBe(
      false,
    );
    h.video.push({ channelReady: { purpose: 3 } });
    await until(() =>
      h.statuses.some((status) => status.includes("通道已配对")),
    );
  } finally {
    await h.dispose();
  }
});

test("DesktopSession retires a codec when its support probe stalls", async () => {
  const codec = "hvc1.1.6.L123.B0";
  const h = harness({ fastDecodeTimeout: true, stalledSupportCodec: codec });
  try {
    await h.ready();
    h.config(2, 2, codec);
    await until(() => h.statuses.includes("正在切换兼容的视频编码…"));
    expect(h.endings).toEqual([]);
    const capabilities = h.control.sent
      .filter((message) => message.desktopVideoCapabilities)
      .at(-1)?.desktopVideoCapabilities;
    expect(capabilities?.codecs).not.toContain(wire.VideoCodec.HEVC);
    h.config(3);
    await until(() => h.decoders.length === 2);
    h.frame(3, 1);
    await until(() => h.decoders[1].chunks.length === 1);
    h.decoders[1].output();
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession falls back when a decoder accepts frames without output", async () => {
  const h = harness({ fastDecodeTimeout: true });
  try {
    await h.ready();
    h.config(2, 2, "hvc1.1.6.L123.B0");
    await until(() => h.decoders.length === 2);
    h.frame(2, 1);
    await until(() => h.statuses.includes("正在切换兼容的视频编码…"));
    expect(h.decoders[1].state).toBe("closed");
    expect(h.endings).toEqual([]);
    h.config(3);
    await until(() => h.decoders.length === 3);
    h.frame(3, 1);
    await until(() => h.decoders[2].chunks.length === 1);
    h.decoders[2].output();
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

test("DesktopSession requests reconnect when the baseline decoder stalls", async () => {
  const h = harness({ fastDecodeTimeout: true });
  try {
    h.control.push({ channelReady: { purpose: 2 } });
    h.video.push({ channelReady: { purpose: 3 } });
    h.config(1);
    await until(() => h.decoders.length === 1);
    h.frame(1, 1);
    await until(() => h.endings.length === 1);
    expect(h.endings).toEqual([true]);
    expect(h.statuses.at(-1)).toContain("视频解码首帧超时");
  } finally {
    await h.dispose();
  }
});

test("DesktopSession delayed detection preserves a codec rejection", async () => {
  const h = harness({ deferredCodecDetection: true });
  try {
    await h.ready();
    h.config(2, 2, "hvc1.1.6.L123.B0");
    await until(() => h.decoders.length === 2);
    h.decoders[1].error();
    h.releaseCodecDetection();
    await new Promise((resolve) => setTimeout(resolve, 0));
    const capabilities = h.control.sent
      .filter((message) => message.desktopVideoCapabilities)
      .at(-1)?.desktopVideoCapabilities;
    expect(capabilities?.codecs).not.toContain(wire.VideoCodec.HEVC);
    expect(capabilities?.codecs).toContain(wire.VideoCodec.AV1);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

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

test("DesktopSession negotiates video only and acknowledges decoded repeats", async () => {
  const h = harness();
  try {
    await h.activate(true);
    const capabilities = h.control.sent
      .filter((message) => message.desktopVideoCapabilities)
      .at(-1)?.desktopVideoCapabilities;
    expect(capabilities?.continuousFrames).toBe(true);
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
    expect(h.draws).toEqual(["video", "video"]);
    const feedback = h.control.sent
      .filter((message) => message.qualityFeedback)
      .at(-1)?.qualityFeedback;
    expect(feedback?.displayedFrame.toString()).toBe("2");
    h.frame(2, 3);
    await until(() => h.decoders[1].chunks.length === 3);
    h.decoders[1].output();
    expect(h.draws).toEqual(["video", "video", "video"]);
    expect(h.endings).toEqual([]);
  } finally {
    await h.dispose();
  }
});

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
        videoFrame: {
          timestampMicroseconds: 2,
          keyFrame: true,
          data: new Uint8Array([1]),
        },
      });
      await until(() => h.endings.length === 1);
    } finally {
      await h.dispose();
    }
  }
});
