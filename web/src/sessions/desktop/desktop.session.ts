import { ultraterimal } from "../../generated/session";
import type { DesktopTicket, DesktopPerformance } from "./desktop.types";
import {
  DesktopVisibleFrameGaps,
  visibleFrameTiming,
} from "./visible_frames.session";
import { DesktopInput } from "./input.session";
import { desktopShortcuts } from "./shortcuts.protocol";
import { DesktopOutbound } from "./outbound.session";
import { DesktopVideoContract, desktopDuration } from "./desktop.protocol";
import { desktopCodecFamily, detectDesktopCodecs } from "./codecs.session";
import { DesktopAudioPlayer, type DesktopAudioState } from "./audio.session";
import {
  DesktopVideoDecodeLease,
  DesktopVideoDecodeTimeoutError,
  DesktopVideoMessageReservation,
  DesktopVideoReceiveChannel,
  maximumPendingVideoFrames,
  maximumVideoQueueMessages,
  videoDecodeSlotTimeoutMilliseconds,
} from "./video-queue.session";

const wire = ultraterimal.wire;
type Message = ultraterimal.wire.Envelope;
type Outbound = ultraterimal.wire.IEnvelope;
interface Channel {
  socket: WebSocket;
  id: string;
  purpose: 2 | 3;
  ready: boolean;
  received: number;
  sent: number;
  receive: DesktopVideoReceiveChannel;
}
function integer(raw: number | { toString(): string }, minimum = 0): number {
  const value = typeof raw === "number" ? raw : Number(raw.toString());
  if (!Number.isSafeInteger(value) || value < minimum)
    throw new Error("桌面消息计数无效");
  return value;
}

export class DesktopSession {
  private channels: Channel[] = [];
  private writers = new Map<Channel, DesktopOutbound<Outbound>>();
  private stopped = false;
  private stopping = false;
  private input: DesktopInput | null = null;
  private decoder: VideoDecoder | null = null;
  private generation = 0;
  private frame = 0;
  private displayedContentFrame = 0;
  private readonly continuousReadiness = new Set<number>();
  private pending = new Map<
    number,
    {
      id: number;
      content: number;
      repeated: boolean;
      started: number;
      decodeStarted: number;
      receiveQueueMicroseconds: number;
      lease: DesktopVideoDecodeLease;
    }
  >();
  private videoChannel: DesktopVideoReceiveChannel | null = null;
  private deadline: ReturnType<typeof setTimeout> | undefined;
  private decodeDeadline: ReturnType<typeof setTimeout> | undefined;
  private cancelDecoderProbe: (() => void) | undefined;
  private readonly rejectedCodecs = new Set<ultraterimal.wire.VideoCodec>();
  private livenessDeadline: ReturnType<typeof setTimeout> | undefined;
  private cursorShape: ImageData | null = null;
  private hotspot = { x: 0, y: 0 };
  private cursor = { x: 0, y: 0, visible: false };
  private drawnCursor: { x: number; y: number; shape: ImageData } | null = null;
  private readonly cursorDataUrlCache = new Map<string, string>();
  private context: CanvasRenderingContext2D;
  private cursorContext: CanvasRenderingContext2D;
  private viewportResize: ResizeObserver | undefined;
  private visibility = new AbortController();
  private displayedFrame = 0;
  private observedFrame = 0;
  private readonly visibleFrameGaps = new DesktopVisibleFrameGaps();
  private renderReportAt = 0;
  private observationAnimation: number | undefined;
  private visible = true;
  private scaleMode: "native" | "fit" = "fit";
  private decodeMicroseconds = 0;
  private resizeSequence = 0;
  private resizePending = false;
  private queuedResize: { width: number; height: number } | null = null;
  private lastProbeNonce = 0;
  private lastAcceptedResponseNonce = 0;
  private resizeDeadline: ReturnType<typeof setTimeout> | undefined;
  private startedAt = performance.now();
  private firstFrameMilliseconds: number | null = null;
  private displayedCount = 0;
  private presentedCount = 0;
  private decodeTotal = 0;
  private codec = "";
  private readonly optimization = new DesktopVideoContract();
  private optimizationV2 = false;
  private readonly videoGenerations = new Map<
    number,
    {
      frame: number;
      content: number;
      continuous: boolean;
      timestamp: number;
    }
  >();
  private browserTiming: ultraterimal.wire.IDesktopBrowserTiming | undefined;
  private supportedCodecs: ultraterimal.wire.VideoCodec[] = [
    wire.VideoCodec.AVC,
  ];
  private rejectedGeneration = 0;
  private capabilitiesAccepted = false;
  private videoBytes = 0;
  private bitrateSince = 0;
  private bitrateTimer: ReturnType<typeof setTimeout> | undefined;
  private readonly audio: DesktopAudioPlayer;
  private audioMuted: boolean;
  private serverDesktopAudio = false;
  private agentDesktopAudio = false;
  private lastSentAudioEnabled: boolean | null = null;

  constructor(
    private ticket: DesktopTicket,
    private canvas: HTMLCanvasElement,
    private overlay: HTMLCanvasElement,
    private status: (message: string) => void,
    private ended: (hadFrame: boolean, retryable: boolean) => void,
    private displayModes: (
      modes: { width: number; height: number }[],
    ) => void = () => {},
    private bitrate: (bitsPerSecond: number) => void = () => {},
    private performanceSample: (sample: DesktopPerformance) => void = () => {},
    initialMuted = false,
    audioState: (state: DesktopAudioState) => void = () => {},
  ) {
    this.audioMuted = initialMuted;
    this.audio = new DesktopAudioPlayer(initialMuted, audioState);
    const context = canvas.getContext("2d", {
      alpha: false,
      // Keep whole-frame presentation in the compositor's normal paint cycle.
      // A desynchronized canvas can tear across scrolling desktop content.
      desynchronized: false,
    });
    const cursorContext = overlay.getContext("2d");
    if (!context || !cursorContext || !globalThis.VideoDecoder)
      throw new Error(
        "当前浏览器不支持桌面视频解码，请使用新版 Chrome 或 Edge",
      );
    this.context = context;
    this.cursorContext = cursorContext;
    try {
      this.viewportResize = new ResizeObserver(() => {
        this.sizeDesktop();
        this.updateLocalCursor();
      });
      this.viewportResize.observe(canvas);
      this.observePixelRatio();
      const viewport = canvas.closest("[data-desktop-viewport]");
      if (viewport) this.viewportResize.observe(viewport);
      this.deadline = setTimeout(() => this.fail("桌面首帧超时", true), 60000);
      document.addEventListener(
        "visibilitychange",
        () => {
          this.visibleFrameGaps.clear();
          this.reportDisplay();
        },
        {
          signal: this.visibility.signal,
        },
      );
      for (const grant of ticket.channels) {
        const url = new URL(
          grant.purpose === 2 ? "/api/desktop/control" : "/api/desktop/video",
          location.href,
        );
        url.protocol = "wss:";
        const socket = new WebSocket(url);
        const channel: Channel = {
          socket,
          id: grant.id,
          purpose: grant.purpose,
          ready: false,
          received: 0,
          sent: 0,
          receive: new DesktopVideoReceiveChannel(grant.purpose === 3),
        };
        if (grant.purpose === 3) this.videoChannel = channel.receive;
        this.channels.push(channel);
        socket.binaryType = "arraybuffer";
        socket.onopen = () =>
          this.send(channel, {
            channelOpen: {
              purpose: grant.purpose,
              ticket: new TextEncoder().encode(grant.ticket),
            },
          });
        socket.onmessage = (event: MessageEvent<unknown>) => {
          const receivedAt = performance.now();
          const data = event.data;
          if (!(data instanceof ArrayBuffer)) {
            this.fail("桌面接收队列超出限制");
            return;
          }
          let probedConfiguration: Message | undefined;
          if (
            !channel.receive.enqueue(
              data,
              async (reservation) => {
                if (this.stopped) return;
                await this.receive(
                  channel,
                  probedConfiguration ??
                    wire.Envelope.decode(new Uint8Array(data)),
                  reservation,
                  receivedAt,
                );
              },
              (error) =>
                this.fail(
                  error instanceof DesktopVideoDecodeTimeoutError
                    ? "视频解码等待超时，会话已结束"
                    : "桌面数据或解码异常，会话已结束",
                ),
              () => {
                if (
                  channel.purpose !== 3 ||
                  channel.receive.decodeWaiterCount === 0
                )
                  return;
                try {
                  const candidate = wire.Envelope.decode(new Uint8Array(data));
                  if (candidate.body !== "videoConfig") return;
                  const candidateGeneration = integer(candidate.generation, 1);
                  if (candidateGeneration <= this.generation) return;
                  probedConfiguration = candidate;
                  channel.receive.announceVideoGeneration(
                    candidateGeneration,
                    this.generation,
                  );
                } catch {
                  // The serialized receive path reports malformed messages.
                }
              },
            )
          )
            this.fail("桌面接收队列超出限制");
        };
        socket.onerror = () => this.fail("桌面连接失败", true);
        socket.onclose = (event) =>
          this.fail(
            `桌面连接已断开（通道 ${channel.purpose}，代码 ${event.code}）`,
            true,
          );
      }
      void detectDesktopCodecs().then((codecs) => {
        if (this.stopped) return;
        this.supportedCodecs = codecs.filter(
          (codec) => !this.rejectedCodecs.has(codec),
        );
        this.reportCapabilities();
      });
    } catch (error) {
      this.stop();
      throw error;
    }
  }

  private send(channel: Channel, body: Outbound): void {
    if (this.stopped) return;
    let writer = this.writers.get(channel);
    if (!writer) {
      writer = new DesktopOutbound(
        channel.socket,
        (message) =>
          new Uint8Array(
            wire.Envelope.encode({
              ...message,
              protocolVersion: 1,
              sessionId: this.ticket.id,
              channelId: channel.id,
              sequence: integer(++channel.sent, 1),
            }).finish(),
          ),
        (reason) => this.fail(`桌面发送拥塞或连接已断开（${reason}）`, true),
      );
      this.writers.set(channel, writer);
    }
    writer.send(
      body,
      body.qualityFeedback
        ? "feedback"
        : body.inputEvent?.kind === wire.InputEvent.Kind.POINTER_MOVE
          ? "pointer"
          : "",
    );
  }
  resize(width: number, height: number): void {
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (
      !channel ||
      this.stopped ||
      !this.ticket.writeControl ||
      !this.generation
    )
      return;
    if (
      !Number.isInteger(width) ||
      !Number.isInteger(height) ||
      width < 16 ||
      height < 16 ||
      width > 4096 ||
      height > 2160 ||
      width % 2 ||
      height % 2
    )
      return;
    if (this.resizePending) {
      this.queuedResize = { width, height };
      return;
    }
    this.queuedResize = null;
    this.resizePending = true;
    const requestId = ++this.resizeSequence;
    this.status("正在调整远程分辨率…");
    this.resizeDeadline = setTimeout(() => {
      this.resizePending = false;
      this.status("分辨率调整尚未确认，请检查当前画面");
    }, 15000);
    this.send(channel, { desktopResize: { requestId, width, height } });
  }
  shortcut(id: string): boolean {
    if (this.stopped || !this.visible || !this.ticket.writeControl)
      return false;
    const shortcut = desktopShortcuts.find((item) => item.id === id);
    if (!shortcut) return false;
    return shortcut.id === "secure-attention"
      ? !!this.input?.secureAttention()
      : !!this.input?.shortcut(shortcut.keys);
  }
  setAudioMuted(muted: boolean): void {
    if (this.stopped) return;
    this.audioMuted = muted;
    this.audio.setMuted(muted);
    this.syncAudioControl();
  }
  unlockAudio(): void {
    if (this.stopped) return;
    void this.audio.unlock();
  }
  private syncAudioControl(): void {
    if (this.stopped || !this.serverDesktopAudio || !this.agentDesktopAudio)
      return;
    this.audio.setSupported();
    if (!this.audio.isSupported) return;
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (!channel) return;
    const enabled = !this.audioMuted;
    if (this.lastSentAudioEnabled === enabled) return;
    this.lastSentAudioEnabled = enabled;
    this.send(channel, { audioControl: { enabled } });
  }
  setVisible(visible: boolean): void {
    if (this.stopped || this.visible === visible) return;
    this.visible = visible;
    this.visibleFrameGaps.clear();
    if (!visible) {
      this.input?.release();
    } else {
      this.sizeDesktop();
      this.updateLocalCursor();
    }
    this.reportDisplay();
  }
  private reportDisplay(
    renderReport?: ultraterimal.wire.IVisibleFrameTiming,
  ): void {
    if (this.stopped || !this.displayedFrame) return;
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (channel)
      this.send(channel, {
        qualityFeedback: {
          displayedGeneration: this.generation,
          displayedFrame: this.displayedFrame,
          decodeMicroseconds: this.decodeMicroseconds,
          queueDepth:
            this.pending.size +
            (this.videoChannel?.budget.queuedMessageCount ?? 0),
          background: document.hidden || !this.visible,
          maximumPendingVideoFrames,
          ...(renderReport ? { visibleFrameTiming: renderReport } : {}),
          ...(this.optimizationV2 && this.browserTiming
            ? { browserTiming: this.browserTiming }
            : {}),
        },
      });
  }
  private reportCapabilities(): void {
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (this.stopped || !channel || !this.capabilitiesAccepted) return;
    const contract = this.optimization.request();
    this.send(channel, {
      desktopVideoCapabilities: {
        codecs: this.supportedCodecs,
        desktopOptimizationContract: contract,
        continuousFrames: contract === 2 && this.continuousReadiness.size === 2,
      },
    });
  }
  private rejectCodec(codec: string, timedOut = false): void {
    const family = desktopCodecFamily(codec);
    clearTimeout(this.decodeDeadline);
    this.decodeDeadline = undefined;
    if (
      family === wire.VideoCodec.AVC ||
      family === wire.VideoCodec.VIDEO_CODEC_UNSPECIFIED
    ) {
      this.fail(timedOut ? "视频解码首帧超时" : "视频解码失败", timedOut);
      return;
    }
    this.rejectedGeneration = this.generation;
    this.rejectedCodecs.add(family);
    this.supportedCodecs = this.supportedCodecs.filter(
      (entry) => entry !== family,
    );
    if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
    this.decoder = null;
    this.videoChannel?.resetVideoGeneration();
    for (const pending of this.pending.values()) pending.lease.release();
    this.pending.clear();
    this.reportCapabilities();
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (channel) this.send(channel, { requestRefresh: {} });
    this.status("正在切换兼容的视频编码…");
  }
  private observeCanvasFrame(): void {
    if (this.observationAnimation !== undefined || this.stopped) return;
    const generation = this.generation;
    this.observationAnimation = requestAnimationFrame((at) => {
      this.observationAnimation = undefined;
      if (
        this.stopped ||
        generation !== this.generation ||
        document.hidden ||
        !this.visible
      )
        return;
      if (this.displayedFrame > this.observedFrame) {
        this.observedFrame = this.displayedFrame;
        this.presentedCount++;
        this.visibleFrameGaps.observe(at);
      }
    });
  }
  private async receive(
    channel: Channel,
    message: Message,
    reservation: DesktopVideoMessageReservation,
    receivedAt: number,
  ): Promise<void> {
    const sequence = integer(message.sequence);
    const generationId = integer(message.generation);
    const frameId = integer(message.frameId);
    const baseFrameId = integer(message.baseFrameId);
    if (
      message.protocolVersion !== 1 ||
      message.sessionId !== this.ticket.id ||
      message.channelId !== channel.id ||
      integer(sequence, 1) <= channel.received
    )
      throw new Error("Desktop binding mismatch");
    channel.received = sequence;
    if (!channel.ready) {
      if (
        sequence !== 1 ||
        message.body !== "channelReady" ||
        message.channelReady?.purpose !== channel.purpose ||
        generationId ||
        frameId ||
        baseFrameId
      )
        throw new Error("Desktop not ready");
      channel.ready = true;
      this.optimization.ready(
        channel.purpose,
        message.channelReady?.desktopOptimizationContract ?? 0,
      );
      if (message.channelReady.continuousFrames)
        this.continuousReadiness.add(channel.purpose);
      else this.continuousReadiness.delete(channel.purpose);
      this.reportCapabilities();
      if (this.channels.every((entry) => entry.ready) && !this.generation)
        this.status("通道已配对，等待远端采集与视频首帧…");
      if (channel.purpose === 2) {
        this.serverDesktopAudio = !!message.channelReady.desktopAudio;
        this.syncAudioControl();
        this.reportCapabilities();
        // The video socket may display a frame before this input socket pairs.
        // Never advertise writable input while its ordered channel is absent.
        this.reportDisplay();
        if (this.stopped) return;
        this.completeStartup();
        if (this.displayedFrame) {
          this.bindInput();
          if (this.input || !this.ticket.writeControl)
            this.status(
              `${this.overlay.width} × ${this.overlay.height} · ${this.ticket.writeControl ? "可控制" : "只读连接"}`,
            );
        }
      }
      return;
    }
    const retired =
      generationId < this.generation ||
      channel.receive.isRetiredGeneration(generationId, this.generation);
    const videoGeneration = this.videoGenerations.get(generationId);
    if (channel.purpose === 2) {
      if (message.body === "desktopProbe" && message.desktopProbe) {
        const probe = wire.DesktopProbe.create(message.desktopProbe);
        const nonce = integer(probe.nonce, 1);
        const acceptedResponseNonce = integer(probe.acceptedResponseNonce);
        if (
          nonce > 0xffffffff ||
          nonce <= this.lastProbeNonce ||
          acceptedResponseNonce >= nonce ||
          acceptedResponseNonce < this.lastAcceptedResponseNonce ||
          generationId ||
          frameId ||
          baseFrameId
        )
          throw new Error("Invalid desktop liveness probe");
        this.lastProbeNonce = nonce;
        this.lastAcceptedResponseNonce = acceptedResponseNonce;
        // Older workers may ask us to release held state. Slow probes never disable input.
        if (probe.resetInput) this.input?.release();
        if (this.stopped) return;
        this.armLivenessDeadline();
        this.send(channel, { desktopProbe: { nonce } });
        return;
      }
      if (
        message.body === "desktopResizeResult" &&
        message.desktopResizeResult
      ) {
        const result = wire.DesktopResizeResult.create(
          message.desktopResizeResult,
        );
        const requestId = integer(result.requestId, 1);
        if (
          generationId ||
          frameId ||
          baseFrameId ||
          requestId > this.resizeSequence
        )
          throw new Error("Invalid resize result");
        if (requestId === this.resizeSequence) {
          clearTimeout(this.resizeDeadline);
          this.resizePending = false;
          const next = this.queuedResize;
          this.queuedResize = null;
          if (next) {
            this.resize(next.width, next.height);
            return;
          }
          this.status(
            result.applied
              ? "分辨率已应用，正在更新画面…"
              : "Windows 未能应用此分辨率，原会话保持连接",
          );
        }
        return;
      }
      if (
        message.body === "desktopSecureAttentionResult" &&
        message.desktopSecureAttentionResult
      ) {
        if (generationId || frameId || baseFrameId)
          throw new Error("Invalid secure attention result");
        const error = message.desktopSecureAttentionResult.win32Error ?? 0;
        this.status(
          error === 0
            ? "Ctrl+Alt+Del 已发送"
            : error === 1260
              ? "Windows 策略禁止发送 Ctrl+Alt+Del"
              : error === 1237
                ? "操作过于频繁，请稍后再试"
                : `Ctrl+Alt+Del 发送失败（${error}）`,
        );
        return;
      }
      if (message.body === "audioConfig" && message.audioConfig) {
        if (generationId || frameId || baseFrameId)
          throw new Error("Invalid audio config");
        const audioConfig = wire.AudioConfig.create(message.audioConfig);
        if (
          audioConfig.available &&
          (audioConfig.codec !== "opus" ||
            audioConfig.sampleRate !== 48000 ||
            audioConfig.channels < 1 ||
            audioConfig.channels > 2)
        )
          throw new Error("Unsupported audio config");
        await this.audio.configure({
          available: audioConfig.available,
          codec: audioConfig.codec,
          sampleRate: audioConfig.sampleRate,
          channels: audioConfig.channels,
        });
        return;
      }
      if (message.body === "audioFrame" && message.audioFrame) {
        if (generationId || frameId || baseFrameId)
          throw new Error("Invalid audio frame");
        const audioFrame = wire.AudioFrame.create(message.audioFrame);
        if (!audioFrame.data.length) throw new Error("Empty audio frame");
        this.audio.decodeFrame(
          new Uint8Array(audioFrame.data),
          integer(audioFrame.timestampMicroseconds),
        );
        return;
      }
      if (message.body !== "cursorUpdate" || !message.cursorUpdate)
        throw new Error("Invalid cursor channel");
      const cursor = wire.CursorUpdate.create(message.cursorUpdate);
      const rgba = cursor.rgba;
      this.cursor = { x: cursor.x, y: cursor.y, visible: cursor.visible };
      if (rgba.length) {
        const width = integer(cursor.width, 1),
          height = integer(cursor.height, 1);
        if (
          width > 1024 ||
          height > 1024 ||
          rgba.length !== width * height * 4 ||
          cursor.hotspotX >= width ||
          cursor.hotspotY >= height
        )
          throw new Error("Invalid cursor shape");
        this.cursorShape = new ImageData(
          new Uint8ClampedArray(rgba),
          width,
          height,
        );
        this.hotspot = { x: cursor.hotspotX, y: cursor.hotspotY };
      }
      this.updateLocalCursor();
      this.drawCursor();
      return;
    }
    if (message.body === "videoConfig" && message.videoConfig) {
      const video = wire.VideoConfig.create(message.videoConfig);
      this.capabilitiesAccepted =
        video.desktopOptimization || video.desktopOptimizationContract === 2;
      const sessionFeatures = integer(video.sessionCapabilities?.features ?? 0);
      this.agentDesktopAudio = (sessionFeatures & 2) !== 0;
      this.syncAudioControl();
      const modes = video.displayModes.map((mode) =>
        wire.DisplayMode.create(mode),
      );
      if (
        modes.length > 128 ||
        modes.some(
          (mode) =>
            !Number.isInteger(mode.width) ||
            !Number.isInteger(mode.height) ||
            mode.width < 16 ||
            mode.height < 16 ||
            mode.width > 4096 ||
            mode.height > 2160 ||
            mode.width % 2 ||
            mode.height % 2,
        )
      )
        throw new Error("Invalid display modes");
      this.displayModes(
        modes.map((mode) => ({
          width: mode.width,
          height: mode.height,
        })),
      );
      if (
        integer(generationId, 1) <= this.generation ||
        frameId ||
        baseFrameId ||
        integer(video.width, 1) > 16384 ||
        integer(video.height, 1) > 16384 ||
        video.width * video.height > 33554432 ||
        desktopCodecFamily(video.codec) ===
          wire.VideoCodec.VIDEO_CODEC_UNSPECIFIED
      )
        throw new Error("Invalid video configuration");
      // A new configuration confirms that the Agent retired the old input
      // adapter (and released its held state). Drop only unsent old-generation
      // input; already submitted bytes are filtered by the Agent's binding.
      const discardRetiredInput = () => {
        for (const writer of this.writers.values()) {
          writer.discardObsolete(
            (body) =>
              !!body.inputEvent && integer(body.generation ?? 0) < generationId,
          );
        }
      };
      discardRetiredInput();
      this.optimizationV2 = this.optimization.configure(
        video.desktopOptimizationContract,
      );
      this.videoGenerations.set(generationId, {
        frame: 0,
        content: 0,
        continuous:
          this.optimizationV2 &&
          video.continuousFrames &&
          this.continuousReadiness.size === 2,
        timestamp: -1,
      });
      if (this.videoGenerations.size > maximumVideoQueueMessages + 1) {
        const oldest = this.videoGenerations.keys().next().value;
        if (oldest !== undefined) this.videoGenerations.delete(oldest);
      }
      this.browserTiming = undefined;
      this.reportCapabilities();
      channel.receive.beginVideoGeneration(generationId);
      if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
      this.input?.dispose();
      if (this.stopped) return;
      // dispose() may enqueue a release tagged with that same retired binding.
      discardRetiredInput();
      this.input = null;
      this.decoder = null;
      clearTimeout(this.decodeDeadline);
      this.decodeDeadline = undefined;
      this.pending.clear();
      if (this.observationAnimation !== undefined)
        cancelAnimationFrame(this.observationAnimation);
      this.observationAnimation = undefined;
      this.generation = generationId;
      this.displayedFrame = this.observedFrame = 0;
      this.visibleFrameGaps.clear();
      this.frame = 0;
      this.displayedContentFrame = 0;
      if (
        this.overlay.width !== video.width ||
        this.overlay.height !== video.height
      ) {
        this.overlay.width = video.width;
        this.overlay.height = video.height;
        // Resizing a canvas clears it, including any previously drawn cursor.
        this.drawnCursor = null;
      }
      if (this.displayedCount === 0) {
        this.canvas.width = video.width;
        this.canvas.height = video.height;
      }
      this.canvas.dataset.sourceWidth = String(video.width);
      this.canvas.dataset.sourceHeight = String(video.height);
      this.sizeDesktop();
      this.updateLocalCursor();
      this.drawCursor();
      const configuration: VideoDecoderConfig = {
        codec: video.codec,
        codedWidth: video.width,
        codedHeight: video.height,
        optimizeForLatency: true,
      };
      if (video.description.length)
        configuration.description = new Uint8Array(video.description);
      this.status("已收到视频配置，正在检查解码支持…");
      let supportDeadline: ReturnType<typeof setTimeout> | undefined;
      const supported = await Promise.race([
        VideoDecoder.isConfigSupported(configuration).catch(() => ({
          supported: false,
        })),
        new Promise<null>((resolve) => {
          this.cancelDecoderProbe = () => resolve(null);
          supportDeadline = setTimeout(() => resolve(null), 4000);
        }),
      ]).finally(() => {
        clearTimeout(supportDeadline);
        this.cancelDecoderProbe = undefined;
      });
      if (this.stopped) return;
      if (!supported) {
        channel.receive.completeVideoGeneration(generationId);
        this.rejectCodec(video.codec, true);
        return;
      }
      if (!supported.supported) {
        channel.receive.completeVideoGeneration(generationId);
        this.rejectCodec(video.codec);
        return;
      }
      if (this.stopped) return;
      const generation = this.generation;
      this.decoder = new VideoDecoder({
        error: () => {
          if (generation === this.generation) this.rejectCodec(video.codec);
        },
        output: (frame) => {
          const outputAt = performance.now();
          try {
            if (
              this.stopped ||
              generation !== this.generation ||
              generation === this.rejectedGeneration
            )
              return;
            const pending = this.pending.get(frame.timestamp);
            if (
              !pending ||
              frame.displayWidth !== this.overlay.width ||
              frame.displayHeight !== this.overlay.height ||
              !this.videoChannel?.finishFrame(frame.timestamp, pending.lease)
            )
              throw new Error("Unexpected decoded frame");
            this.pending.delete(frame.timestamp);
            const submitStarted = performance.now();
            if (pending.repeated) {
              if (this.displayedContentFrame !== pending.content)
                throw new Error("Repeated video lost its content baseline");
            } else {
              if (
                this.canvas.width !== frame.displayWidth ||
                this.canvas.height !== frame.displayHeight
              ) {
                this.canvas.width = frame.displayWidth;
                this.canvas.height = frame.displayHeight;
              }
              this.context.drawImage(frame, 0, 0);
            }
            const canvasSubmitMicroseconds = desktopDuration(
              performance.now() - submitStarted,
            );
            this.browserTiming = {
              receiveQueueMicroseconds: pending.receiveQueueMicroseconds,
              decodeMicroseconds: desktopDuration(
                outputAt - pending.decodeStarted,
              ),
              canvasSubmitMicroseconds,
            };
            this.canvas.dataset.frame = String(pending.id);
            this.canvas.dataset.generation = String(generation);
            const firstDisplay = !this.displayedFrame;
            this.displayedFrame = pending.id;
            this.displayedContentFrame = pending.content;

            if (firstDisplay) {
              clearTimeout(this.decodeDeadline);
              this.decodeDeadline = undefined;
              this.completeStartup();
              this.bindInput();
              const state =
                this.deadline !== undefined
                  ? "等待控制通道"
                  : this.ticket.writeControl
                    ? this.input
                      ? "可控制"
                      : "等待输入恢复"
                    : "只读连接";
              this.status(
                `${frame.displayWidth} × ${frame.displayHeight} · ${state}`,
              );
            }
            this.decodeMicroseconds = Math.min(
              1000000,
              Math.round((performance.now() - pending.started) * 1000),
            );
            this.firstFrameMilliseconds ??= performance.now() - this.startedAt;
            this.displayedCount++;
            this.decodeTotal += this.decodeMicroseconds / 1000;
            this.recordVideoBytes(0);
            this.reportDisplay();
            if (!document.hidden && this.visible) this.observeCanvasFrame();
          } catch {
            this.fail("桌面显示异常");
          } finally {
            // A retired decoder may reuse a timestamp from the new generation.
            // Its callback must never release the new decoder's slot.
            if (generation === this.generation)
              this.pending.get(frame.timestamp)?.lease.release();
            frame.close();
          }
        },
      });
      this.codec = video.codec;
      this.status("已收到视频配置，等待解码首帧…");
      this.decoder.configure(configuration);
      channel.receive.completeVideoGeneration(generationId);
      return;
    }
    const image = message.videoFrame
      ? wire.VideoFrame.create(message.videoFrame)
      : null;
    const rejected = generationId === this.rejectedGeneration;
    if (
      message.body !== "videoFrame" ||
      !image ||
      !videoGeneration ||
      (generationId !== this.generation &&
        !channel.receive.isRetiredGeneration(generationId, this.generation)) ||
      (!retired && !rejected && !this.decoder) ||
      integer(frameId, 1) <= videoGeneration.frame ||
      (image.repeated
        ? !videoGeneration.continuous ||
          !videoGeneration.content ||
          baseFrameId !== videoGeneration.content
        : !!baseFrameId) ||
      (!videoGeneration.frame && !image.keyFrame) ||
      integer(image.timestampMicroseconds) <= videoGeneration.timestamp ||
      !image.data.length
    )
      throw new Error("Invalid video frame");
    // Remember validated wire baselines even if slot admission is interrupted.
    videoGeneration.frame = frameId;
    if (!image.repeated) videoGeneration.content = frameId;
    videoGeneration.timestamp = integer(image.timestampMicroseconds);
    if (retired || rejected) return;
    if (!this.decoder) throw new Error("Desktop decoder unavailable");
    const timestamp = integer(image.timestampMicroseconds);
    const decoder = this.decoder;
    const generation = this.generation;
    this.recordVideoBytes(image.data.length);
    const submitted = await channel.receive.submitFrame(
      reservation,
      timestamp,
      image.data.length,
      videoDecodeSlotTimeoutMilliseconds,
      () =>
        !this.stopped &&
        this.generation === generation &&
        this.decoder === decoder &&
        !channel.receive.isReconfiguring &&
        decoder.state === "configured",
      (lease) => {
        this.frame = frameId;
        const started = performance.now();
        // Chunk construction copies the payload; keep that cost out of the
        // decode-call -> output-entry stage but retain the legacy aggregate.
        const chunk = new EncodedVideoChunk({
          type: image.keyFrame ? "key" : "delta",
          timestamp,
          data: image.data,
        });
        const decodeStarted = performance.now();
        this.pending.set(timestamp, {
          id: frameId,
          content: videoGeneration.content,
          repeated: image.repeated,
          started,
          decodeStarted,
          receiveQueueMicroseconds: desktopDuration(decodeStarted - receivedAt),
          lease,
        });
        decoder.decode(chunk);
        if (!this.displayedFrame && this.decodeDeadline === undefined) {
          this.decodeDeadline = setTimeout(() => {
            this.decodeDeadline = undefined;
            if (
              !this.stopped &&
              this.decoder === decoder &&
              generation === this.generation &&
              !this.displayedFrame
            )
              this.rejectCodec(this.codec, true);
          }, 4000);
        }
      },
    );
    if (submitted === "cancelled") {
      if (
        this.stopped ||
        channel.receive.isRetiredGeneration(generationId, generation) ||
        generationId === this.rejectedGeneration ||
        generation !== this.generation
      )
        return;
      throw new Error("Video decode admission was cancelled unexpectedly");
    }
  }
  private drawCursor(): void {
    const shape =
      !this.ticket.writeControl && this.cursor.visible
        ? this.cursorShape
        : null;
    const x = this.cursor.x - this.hotspot.x;
    const y = this.cursor.y - this.hotspot.y;
    const previous = this.drawnCursor;
    if (
      previous &&
      previous.shape === shape &&
      previous.x === x &&
      previous.y === y
    )
      return;
    if (previous) {
      this.cursorContext.clearRect(
        previous.x,
        previous.y,
        previous.shape.width,
        previous.shape.height,
      );
      this.drawnCursor = null;
    }
    if (!shape) return;
    this.cursorContext.putImageData(shape, x, y);
    this.drawnCursor = { x, y, shape };
  }
  private recordVideoBytes(bytes: number): void {
    this.videoBytes += bytes;
    if (this.bitrateTimer !== undefined) return;
    this.bitrateSince = performance.now();
    this.bitrateTimer = setTimeout(() => this.publishBitrate(), 1000);
  }
  private publishBitrate(): void {
    this.bitrateTimer = undefined;
    if (this.stopped) return;
    const now = performance.now();
    const bytes = this.videoBytes;
    this.videoBytes = 0;
    const elapsed = Math.max(1, now - this.bitrateSince);
    const frames = this.displayedCount;
    const presentedFrames = this.presentedCount;
    const decodeMilliseconds = frames ? this.decodeTotal / frames : null;
    // An unchanged desktop is not a stalled video stream. Avoid carrying its
    // idle time into the next visible frame or retaining stale gap metrics.
    if (!frames && !presentedFrames) this.visibleFrameGaps.clear();
    const visibleGaps = this.visibleFrameGaps.summary();
    if (now - this.renderReportAt >= 5000 && this.visible && !document.hidden) {
      const timing = visibleFrameTiming(
        visibleGaps,
        (presentedFrames * 1000) / elapsed,
      );
      if (timing) this.reportDisplay(timing);
      this.renderReportAt = now;
    }
    this.bitrate((bytes * 8000) / elapsed);
    this.performanceSample({
      // Only count decoded images observed in a visible rAF, not every
      // drawImage submission that may be overwritten before a paint cycle.
      framesPerSecond: (presentedFrames * 1000) / elapsed,
      decodeMilliseconds,
      pendingFrames: this.pending.size,
      bufferedBytes: this.channels.reduce(
        (sum, channel) => sum + channel.socket.bufferedAmount,
        0,
      ),
      receivedQueueBytes: this.channels.reduce(
        (sum, channel) => sum + channel.receive.budget.queuedMessageBytes,
        0,
      ),
      firstFrameMilliseconds: this.firstFrameMilliseconds,
      width: this.canvas.width,
      height: this.canvas.height,
      codec: this.codec,
    });
    this.displayedCount = this.presentedCount = this.decodeTotal = 0;
    this.bitrateSince = now;
    // Publish one zero sample after traffic/display stops, then remain idle.
    if (bytes || frames || presentedFrames)
      this.bitrateTimer = setTimeout(() => this.publishBitrate(), 1000);
  }
  private sizeDesktop(): void {
    const stage = this.canvas.parentElement;
    if (!stage) return;
    // CSS pixels already include OS scaling and browser zoom. Cancel that
    // scale so each decoded pixel occupies one physical display pixel.
    const ratio = window.devicePixelRatio || 1;
    const viewport = stage.parentElement;
    let width = this.overlay.width / ratio;
    let height = this.overlay.height / ratio;
    if (this.scaleMode === "fit" && viewport && width && height) {
      if (!viewport.clientWidth || !viewport.clientHeight) return;
      const scale = Math.min(
        viewport.clientWidth / width,
        viewport.clientHeight / height,
      );
      width *= scale;
      height *= scale;
      // Snap sub-2px rounding seams (from even-pixel video dimensions) flush to
      // the viewport so auto-fit mode leaves zero black border lines.
      if (
        Math.abs(width - viewport.clientWidth) <= 2 &&
        Math.abs(height - viewport.clientHeight) <= 2
      ) {
        width = viewport.clientWidth;
        height = viewport.clientHeight;
      }
    }
    stage.style.width = `${width}px`;
    stage.style.height = `${height}px`;
  }
  setScaleMode(mode: "native" | "fit"): void {
    this.scaleMode = mode;
    this.sizeDesktop();
    this.updateLocalCursor();
  }
  private observePixelRatio(): void {
    const query = window.matchMedia(
      `(resolution: ${window.devicePixelRatio}dppx)`,
    );
    query.addEventListener(
      "change",
      () => {
        this.sizeDesktop();
        this.updateLocalCursor();
        this.observePixelRatio();
      },
      { once: true, signal: this.visibility.signal },
    );
  }
  private updateLocalCursor(): void {
    if (!this.ticket.writeControl || this.stopped) return;
    if (!this.cursor.visible) {
      this.canvas.style.setProperty("--desktop-cursor", "none");
      return;
    }
    if (!this.cursorShape) return;
    const shape = this.cursorShape;
    const bounds = this.canvas.getBoundingClientRect();
    let scaleX = bounds.width / this.overlay.width;
    let scaleY = bounds.height / this.overlay.height;
    if (scaleX <= 0 || scaleY <= 0) return;
    const limit = Math.min(
      1,
      128 / (shape.width * scaleX),
      128 / (shape.height * scaleY),
    );
    scaleX *= limit;
    scaleY *= limit;
    const renderedWidth = Math.max(1, Math.round(shape.width * scaleX));
    const renderedHeight = Math.max(1, Math.round(shape.height * scaleY));
    const x = Math.min(renderedWidth - 1, Math.round(this.hotspot.x * scaleX));
    const y = Math.min(renderedHeight - 1, Math.round(this.hotspot.y * scaleY));
    let hash = 2166136261;
    const pixels = shape.data;
    for (let index = 0; index < pixels.length; index++) {
      hash ^= pixels[index];
      hash = Math.imul(hash, 16777619);
    }
    const cacheKey = `${shape.width}x${shape.height}->${renderedWidth}x${renderedHeight}@${x},${y}#${hash >>> 0}`;
    let cssCursor = this.cursorDataUrlCache.get(cacheKey);
    if (!cssCursor) {
      const source = document.createElement("canvas");
      source.width = shape.width;
      source.height = shape.height;
      source.getContext("2d")?.putImageData(shape, 0, 0);
      const rendered = document.createElement("canvas");
      rendered.width = renderedWidth;
      rendered.height = renderedHeight;
      rendered
        .getContext("2d")
        ?.drawImage(source, 0, 0, renderedWidth, renderedHeight);
      cssCursor = `url("${rendered.toDataURL()}") ${x} ${y}, default`;
      if (this.cursorDataUrlCache.size >= 16) {
        const oldest = this.cursorDataUrlCache.keys().next().value;
        if (oldest !== undefined) this.cursorDataUrlCache.delete(oldest);
      }
      this.cursorDataUrlCache.set(cacheKey, cssCursor);
    }
    this.canvas.style.setProperty("--desktop-cursor", cssCursor);
  }
  private completeStartup(): void {
    if (
      this.stopped ||
      this.deadline === undefined ||
      !this.displayedFrame ||
      !this.channels.some((entry) => entry.purpose === 2 && entry.ready)
    )
      return;
    clearTimeout(this.deadline);
    this.deadline = undefined;
    if (!this.lastProbeNonce) {
      this.armLivenessDeadline();
    }
  }
  private bindInput(): void {
    if (
      this.stopped ||
      !this.ticket.writeControl ||
      !this.displayedFrame ||
      this.input
    )
      return;
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (!channel) return;
    // Capture this binding, not the later generation when a congested writer flushes.
    const generation = this.generation;
    this.input = new DesktopInput(
      this.canvas,
      (event) => {
        this.send(channel, { generation, inputEvent: event });
      },
      { width: this.overlay.width, height: this.overlay.height },
      () => channel.socket.bufferedAmount > 2048,
    );
  }
  private armLivenessDeadline(): void {
    if (this.stopped) return;
    clearTimeout(this.livenessDeadline);
    this.livenessDeadline = setTimeout(
      () => this.fail("远控存活确认超时", true),
      60000,
    );
  }
  private fail(message: string, retryable = false): void {
    if (this.stopped || this.stopping) return;
    console.warn("Desktop session ended", {
      session: this.ticket.id,
      reason: message,
      generation: this.generation,
      codec: this.codec,
      pendingDecodeFrames: this.pending.size,
      channels: this.channels.map((channel) => ({
        purpose: channel.purpose,
        ready: channel.ready,
        bufferedBytes: channel.socket.bufferedAmount,
        queuedMessages: channel.receive.budget.queuedMessageCount,
        queuedBytes: channel.receive.budget.queuedMessageBytes,
        pendingVideoFrames: channel.receive.budget.pendingFrameCount,
        pendingVideoBytes: channel.receive.budget.pendingFrameBytes,
      })),
    });
    const hadFrame = this.firstFrameMilliseconds !== null;
    const phase = hadFrame
      ? ""
      : !this.channels.every((channel) => channel.ready)
        ? "（通道配对阶段）"
        : !this.generation
          ? "（远端首帧生成阶段）"
          : !this.decoder
            ? "（解码能力检查阶段）"
            : "（首帧接收/解码阶段）";
    this.stop();
    this.status(message + phase);
    this.ended(hadFrame, retryable);
  }
  stop(): void {
    if (this.stopped || this.stopping) return;
    this.stopping = true;
    this.stopped = true;
    this.audio.stop();
    this.cancelDecoderProbe?.();
    this.videoGenerations.clear();

    for (const writer of this.writers.values()) writer.stop();
    this.writers.clear();
    this.input?.dispose();
    this.input = null;
    this.viewportResize?.disconnect();
    this.visibility.abort();
    if (this.observationAnimation !== undefined)
      cancelAnimationFrame(this.observationAnimation);
    this.observationAnimation = undefined;
    this.visibleFrameGaps.clear();
    this.cursorDataUrlCache.clear();
    this.canvas.style.removeProperty("--desktop-cursor");
    clearTimeout(this.deadline);
    clearTimeout(this.livenessDeadline);
    clearTimeout(this.decodeDeadline);
    this.decodeDeadline = undefined;
    this.livenessDeadline = undefined;
    clearTimeout(this.resizeDeadline);
    clearTimeout(this.bitrateTimer);
    this.bitrateTimer = undefined;
    this.videoBytes = 0;
    for (const { socket, receive } of this.channels) {
      receive.close();
      socket.onopen = socket.onmessage = socket.onerror = socket.onclose = null;
      socket.close();
    }
    if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
    this.decoder = null;
    this.pending.clear();
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
    this.cursorContext.clearRect(0, 0, this.overlay.width, this.overlay.height);
    this.drawnCursor = null;
  }
}
