export type DesktopAudioState =
  "unsupported" | "unavailable" | "muted" | "blocked" | "playing";

export interface DesktopAudioConfiguration {
  available: boolean;
  codec: string;
  sampleRate: number;
  channels: number;
}

type AudioContextConstructor = typeof AudioContext;

function resolveAudioContext(): AudioContextConstructor | undefined {
  const globalAudio = globalThis as typeof globalThis & {
    webkitAudioContext?: AudioContextConstructor;
  };
  return globalAudio.AudioContext ?? globalAudio.webkitAudioContext;
}

export class DesktopAudioPlayer {
  private supported = false;
  private codecUnsupported = false;
  private available = true;
  private stopped = false;
  private currentState: DesktopAudioState = "unsupported";
  private context: AudioContext | null = null;
  private gain: GainNode | null = null;
  private decoder: AudioDecoder | null = null;
  private activeSources = new Set<AudioBufferSourceNode>();
  private nextPlaybackTime = 0;
  private unlockController: AbortController | null = null;
  private configureGeneration = 0;

  constructor(
    private muted: boolean,
    private readonly onStateChange: (
      state: DesktopAudioState,
    ) => void = () => {},
  ) {}

  get state(): DesktopAudioState {
    return this.currentState;
  }

  get isSupported(): boolean {
    return this.supported && !this.codecUnsupported;
  }

  setSupported(): void {
    if (this.stopped || this.supported) return;
    if (
      !globalThis.AudioDecoder ||
      !globalThis.EncodedAudioChunk ||
      !resolveAudioContext()
    ) {
      this.codecUnsupported = true;
      this.syncState();
      return;
    }
    this.supported = true;
    if (!this.muted) {
      this.ensureContext();
    }
    this.syncState();
  }

  setMuted(muted: boolean): void {
    if (this.stopped || this.muted === muted) return;
    this.muted = muted;
    if (this.gain) {
      this.gain.gain.value = muted ? 0 : 1;
    }
    if (muted) {
      this.stopActiveSources();
      this.removeUnlockListeners();
    } else if (this.supported && !this.codecUnsupported) {
      this.ensureContext();
      if (this.context?.state === "suspended") {
        void this.unlock();
      }
    }
    this.syncState();
  }

  async unlock(): Promise<void> {
    if (
      this.stopped ||
      !this.supported ||
      this.codecUnsupported ||
      this.muted
    ) {
      return;
    }
    const context = this.ensureContext();
    if (!context) return;
    if (context.state === "suspended") {
      try {
        await context.resume();
      } catch {
        // Browser still requires an explicit user gesture; keep blocked state.
      }
    }
    if (!this.stopped) {
      this.syncState();
    }
  }

  async configure(config: DesktopAudioConfiguration): Promise<void> {
    if (this.stopped || !this.supported || this.codecUnsupported) return;
    const generation = ++this.configureGeneration;
    if (!config.available) {
      this.available = false;
      this.closeDecoder();
      this.stopActiveSources();
      this.syncState();
      return;
    }
    this.available = true;
    const decoderConfig: AudioDecoderConfig = {
      codec: config.codec,
      sampleRate: config.sampleRate,
      numberOfChannels: config.channels,
    };
    const support = await AudioDecoder.isConfigSupported(decoderConfig).catch(
      () => ({ supported: false }),
    );
    if (this.stopped || generation !== this.configureGeneration) return;
    if (!support.supported) {
      this.codecUnsupported = true;
      this.closeDecoder();
      this.stopActiveSources();
      this.syncState();
      return;
    }
    this.closeDecoder();
    this.stopActiveSources();
    if (!this.muted) {
      this.ensureContext();
    }
    const decoder = new AudioDecoder({
      output: (data) => {
        try {
          if (
            this.stopped ||
            decoder !== this.decoder ||
            this.muted ||
            !this.context ||
            !this.gain
          ) {
            return;
          }
          if (this.context.state === "suspended") {
            this.syncState();
            return;
          }
          this.scheduleAudioData(data);
        } finally {
          data.close();
        }
      },
      error: () => {
        if (decoder === this.decoder && !this.stopped) {
          this.available = false;
          this.closeDecoder();
          this.stopActiveSources();
          this.syncState();
        }
      },
    });
    decoder.configure(decoderConfig);
    this.decoder = decoder;
    this.syncState();
  }

  decodeFrame(data: Uint8Array, timestampMicroseconds: number): void {
    if (
      this.stopped ||
      !this.supported ||
      this.codecUnsupported ||
      !this.available ||
      this.muted ||
      !this.decoder ||
      this.decoder.state !== "configured" ||
      !data.length
    ) {
      return;
    }
    if ((this.decoder.decodeQueueSize ?? 0) > 8) {
      return;
    }
    this.decoder.decode(
      new EncodedAudioChunk({
        type: "key",
        timestamp: timestampMicroseconds,
        data,
      }),
    );
  }

  stop(): void {
    if (this.stopped) return;
    this.stopped = true;
    this.configureGeneration++;
    this.removeUnlockListeners();
    this.closeDecoder();
    this.stopActiveSources();
    if (this.context) {
      void this.context.close().catch(() => {});
      this.context = null;
      this.gain = null;
    }
  }

  private ensureContext(): AudioContext | null {
    if (this.context) return this.context;
    const AudioContextImpl = resolveAudioContext();
    if (!AudioContextImpl) return null;
    const context = new AudioContextImpl({
      sampleRate: 48000,
      latencyHint: "interactive",
    });
    const gain = context.createGain();
    gain.gain.value = this.muted ? 0 : 1;
    gain.connect(context.destination);
    this.context = context;
    this.gain = gain;
    return context;
  }

  private closeDecoder(): void {
    if (this.decoder && this.decoder.state !== "closed") {
      try {
        this.decoder.close();
      } catch {
        // Ignore decoder close errors during teardown.
      }
    }
    this.decoder = null;
  }

  private stopActiveSources(): void {
    for (const source of this.activeSources) {
      source.onended = null;
      try {
        source.stop();
      } catch {
        // Ignore sources that already stopped.
      }
      source.disconnect();
    }
    this.activeSources.clear();
    this.nextPlaybackTime = 0;
  }

  private scheduleAudioData(data: AudioData): void {
    if (!this.context || !this.gain) return;
    const frames = data.numberOfFrames;
    const channels = data.numberOfChannels;
    const sampleRate = data.sampleRate;
    if (!frames || !channels || !sampleRate) return;

    const buffer = this.context.createBuffer(channels, frames, sampleRate);
    const format = data.format;
    if (format === "f32-planar") {
      for (let ch = 0; ch < channels; ch++) {
        data.copyTo(buffer.getChannelData(ch), {
          planeIndex: ch,
          format: "f32-planar",
        });
      }
    } else if (format === "f32") {
      const interleaved = new Float32Array(frames * channels);
      data.copyTo(interleaved, { planeIndex: 0, format: "f32" });
      for (let ch = 0; ch < channels; ch++) {
        const channelData = buffer.getChannelData(ch);
        for (let i = 0; i < frames; i++) {
          channelData[i] = interleaved[i * channels + ch];
        }
      }
    } else if (format === "s16-planar") {
      const plane = new Int16Array(frames);
      for (let ch = 0; ch < channels; ch++) {
        data.copyTo(plane, { planeIndex: ch, format: "s16-planar" });
        const channelData = buffer.getChannelData(ch);
        for (let i = 0; i < frames; i++) {
          channelData[i] = plane[i] / 32768;
        }
      }
    } else if (format === "s16") {
      const interleaved = new Int16Array(frames * channels);
      data.copyTo(interleaved, { planeIndex: 0, format: "s16" });
      for (let ch = 0; ch < channels; ch++) {
        const channelData = buffer.getChannelData(ch);
        for (let i = 0; i < frames; i++) {
          channelData[i] = interleaved[i * channels + ch] / 32768;
        }
      }
    } else {
      for (let ch = 0; ch < channels; ch++) {
        data.copyTo(buffer.getChannelData(ch), {
          planeIndex: ch,
          format: "f32-planar",
        });
      }
    }

    const now = this.context.currentTime;
    const minLead = 0.005;
    const targetLead = 0.04;
    const maxLead = 0.16;
    if (this.nextPlaybackTime < now + minLead) {
      this.nextPlaybackTime = now + targetLead;
    } else if (this.nextPlaybackTime > now + maxLead) {
      return;
    }

    const source = this.context.createBufferSource();
    source.buffer = buffer;
    source.connect(this.gain);
    this.activeSources.add(source);
    source.onended = () => {
      this.activeSources.delete(source);
      source.disconnect();
    };
    source.start(this.nextPlaybackTime);
    this.nextPlaybackTime += buffer.duration;
  }

  private installUnlockListeners(): void {
    if (this.unlockController || !globalThis.window?.addEventListener) return;
    this.unlockController = new AbortController();
    const signal = this.unlockController.signal;
    const trigger = () => {
      void this.unlock();
    };
    window.addEventListener("pointerdown", trigger, {
      signal,
      capture: true,
    });
    window.addEventListener("keydown", trigger, {
      signal,
      capture: true,
    });
  }

  private removeUnlockListeners(): void {
    this.unlockController?.abort();
    this.unlockController = null;
  }

  private syncState(): void {
    let next: DesktopAudioState;
    if (!this.supported || this.codecUnsupported) {
      next = "unsupported";
      this.removeUnlockListeners();
    } else if (this.muted) {
      next = "muted";
      this.removeUnlockListeners();
    } else if (!this.available) {
      next = "unavailable";
      this.removeUnlockListeners();
    } else if (!this.context || this.context.state === "suspended") {
      next = "blocked";
      this.installUnlockListeners();
    } else {
      next = "playing";
      this.removeUnlockListeners();
    }

    if (next !== this.currentState) {
      this.currentState = next;
      this.onStateChange(next);
    }
  }
}
