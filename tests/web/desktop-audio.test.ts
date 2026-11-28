import { expect, test } from "bun:test";
import {
  DesktopAudioPlayer,
  type DesktopAudioState,
} from "../../web/src/sessions/desktop/audio.session";

function audioHarness(
  options: {
    initialState?: "running" | "suspended";
    unsupportedDecoder?: boolean;
    configSupported?: boolean;
  } = {},
) {
  const globals = new Map<string, PropertyDescriptor | undefined>();
  const replace = (name: string, value: unknown) => {
    globals.set(name, Object.getOwnPropertyDescriptor(globalThis, name));
    if (value === undefined) {
      Reflect.deleteProperty(globalThis, name);
    } else {
      Object.defineProperty(globalThis, name, {
        value,
        configurable: true,
        writable: true,
      });
    }
  };

  const windowListeners = new Map<string, Set<() => void>>();
  replace("window", {
    addEventListener(
      type: string,
      listener: () => void,
      opts?: { signal?: AbortSignal },
    ) {
      let set = windowListeners.get(type);
      if (!set) {
        set = new Set();
        windowListeners.set(type, set);
      }
      set.add(listener);
      opts?.signal?.addEventListener("abort", () => set?.delete(listener));
    },
  });

  const decoders: MockAudioDecoder[] = [];
  class MockAudioDecoder {
    static async isConfigSupported(config: AudioDecoderConfig) {
      return { supported: options.configSupported ?? true, config };
    }
    state = "unconfigured";
    decodeQueueSize = 0;
    chunks: EncodedAudioChunk[] = [];
    closed = false;
    constructor(private readonly init: AudioDecoderInit) {
      decoders.push(this);
    }
    configure() {
      this.state = "configured";
    }
    decode(chunk: EncodedAudioChunk) {
      this.chunks.push(chunk);
    }
    emitOutput(
      sampleRate: number,
      channels: number,
      frames: number,
      format: AudioSampleFormat = "f32-planar",
      fill = 0.25,
    ) {
      let closedCount = 0;
      const data = {
        format,
        sampleRate,
        numberOfFrames: frames,
        numberOfChannels: channels,
        copyTo(
          dest: ArrayBufferView,
          copyOptions: { planeIndex: number; format?: AudioSampleFormat },
        ) {
          if (dest instanceof Float32Array) {
            dest.fill(fill + copyOptions.planeIndex * 0.1);
          } else if (dest instanceof Int16Array) {
            dest.fill(Math.round(fill * 32768));
          }
        },
        close() {
          closedCount++;
        },
      };
      this.init.output(data as unknown as AudioData);
      return {
        get closedCount() {
          return closedCount;
        },
      };
    }
    close() {
      this.state = "closed";
      this.closed = true;
    }
  }

  const contexts: MockAudioContext[] = [];
  class MockAudioContext {
    state: "running" | "suspended" = options.initialState ?? "running";
    currentTime = 1.0;
    closed = false;
    gainNode = {
      gain: { value: 1 },
      connect() {},
    };
    sources: {
      buffer: {
        duration: number;
        channels: Float32Array[];
        getChannelData(ch: number): Float32Array;
      } | null;
      startedAt: number | null;
      stopped: boolean;
      disconnected: boolean;
      onended: (() => void) | null;
      connect(): void;
      start(when: number): void;
      stop(): void;
      disconnect(): void;
    }[] = [];
    destination = {};
    constructor() {
      contexts.push(this);
    }
    createGain() {
      return this.gainNode;
    }
    createBuffer(channels: number, frames: number, sampleRate: number) {
      const channelData = Array.from(
        { length: channels },
        () => new Float32Array(frames),
      );
      return {
        duration: frames / sampleRate,
        channels: channelData,
        getChannelData(ch: number) {
          return channelData[ch];
        },
      };
    }
    createBufferSource() {
      const source = {
        buffer: null,
        startedAt: null as number | null,
        stopped: false,
        disconnected: false,
        onended: null as (() => void) | null,
        connect() {},
        start(when: number) {
          this.startedAt = when;
        },
        stop() {
          this.stopped = true;
        },
        disconnect() {
          this.disconnected = true;
        },
      };
      this.sources.push(source);
      return source;
    }
    async resume() {
      this.state = "running";
    }
    async close() {
      this.closed = true;
    }
  }

  if (options.unsupportedDecoder) {
    replace("AudioDecoder", undefined);
  } else {
    replace("AudioDecoder", MockAudioDecoder);
  }
  replace(
    "EncodedAudioChunk",
    class {
      timestamp: number;
      data: BufferSource;
      constructor(init: EncodedAudioChunkInit) {
        this.timestamp = init.timestamp;
        this.data = init.data;
      }
    },
  );
  replace("AudioContext", MockAudioContext);

  return {
    decoders,
    contexts,
    windowListeners,
    firePointerDown() {
      for (const listener of windowListeners.get("pointerdown") ?? [])
        listener();
    },
    restore() {
      for (const [name, descriptor] of globals) {
        if (descriptor) Object.defineProperty(globalThis, name, descriptor);
        else Reflect.deleteProperty(globalThis, name);
      }
    },
  };
}

test("DesktopAudioPlayer stays unsupported when WebCodecs AudioDecoder is absent", () => {
  const h = audioHarness({ unsupportedDecoder: true });
  try {
    const states: DesktopAudioState[] = [];
    const player = new DesktopAudioPlayer(false, (s) => states.push(s));
    player.setSupported();
    expect(player.isSupported).toBe(false);
    expect(player.state).toBe("unsupported");
    expect(states).toEqual([]);
    player.stop();
  } finally {
    h.restore();
  }
});

test("DesktopAudioPlayer handles autoplay unlock, gapless scheduling, drift drop and teardown", async () => {
  const h = audioHarness({ initialState: "suspended" });
  try {
    const states: DesktopAudioState[] = [];
    const player = new DesktopAudioPlayer(false, (s) => states.push(s));
    player.setSupported();
    expect(player.state).toBe("blocked");
    expect(h.windowListeners.get("pointerdown")?.size).toBe(1);

    h.firePointerDown();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(player.state).toBe("playing");
    expect(h.windowListeners.get("pointerdown")?.size).toBe(0);

    await player.configure({
      available: true,
      codec: "opus",
      sampleRate: 48000,
      channels: 2,
    });
    expect(h.decoders).toHaveLength(1);
    player.decodeFrame(new Uint8Array([1, 2, 3]), 20000);
    expect(h.decoders[0].chunks).toHaveLength(1);

    const out1 = h.decoders[0].emitOutput(48000, 2, 960, "f32-planar", 0.25);
    const out2 = h.decoders[0].emitOutput(48000, 2, 960, "s16", 0.5);
    expect(out1.closedCount).toBe(1);
    expect(out2.closedCount).toBe(1);

    const ctx = h.contexts[0];
    expect(ctx.sources).toHaveLength(2);
    expect(ctx.sources[0].startedAt).toBeCloseTo(1.04, 6);
    expect(ctx.sources[1].startedAt).toBeCloseTo(1.06, 6);
    expect(ctx.sources[0].buffer?.channels[0][0]).toBeCloseTo(0.25, 5);
    expect(ctx.sources[0].buffer?.channels[1][0]).toBeCloseTo(0.35, 5);
    expect(ctx.sources[1].buffer?.channels[0][0]).toBeCloseTo(0.5, 4);

    // Push playback schedule beyond +160 ms lead to verify drift drop.
    for (let i = 0; i < 10; i++) {
      h.decoders[0].emitOutput(48000, 2, 960);
    }
    expect(ctx.sources.length).toBeLessThanOrEqual(7);

    player.setMuted(true);
    expect(player.state).toBe("muted");
    expect(ctx.gainNode.gain.value).toBe(0);
    expect(ctx.sources.every((s) => s.stopped)).toBe(true);

    player.setMuted(false);
    expect(player.state).toBe("playing");
    expect(ctx.gainNode.gain.value).toBe(1);

    await player.configure({
      available: false,
      codec: "",
      sampleRate: 0,
      channels: 0,
    });
    expect(player.state).toBe("unavailable");

    player.stop();
    expect(ctx.closed).toBe(true);
    expect(h.decoders[0].closed).toBe(true);
  } finally {
    h.restore();
  }
});
