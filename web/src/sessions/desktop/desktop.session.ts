import { ultraterimal } from "../../generated/session";
import type { DesktopTicket, DesktopPerformance } from "./desktop.types";
import { DesktopQualityPolicy, type DesktopQuality } from "./quality.session";
import { DesktopInput } from "./input.session";
import { desktopShortcuts } from "./shortcuts.protocol";
import { DesktopOutbound } from "./outbound.session";

const wire = ultraterimal.wire;
const maximumPendingVideoFrames = 16;
type Message = ultraterimal.wire.Envelope;
type Outbound = ultraterimal.wire.IEnvelope;
interface Channel {
  socket: WebSocket;
  id: string;
  purpose: 2 | 3;
  ready: boolean;
  received: number;
  sent: number;
  queued: number;
  bytes: number;
  work: Promise<void>;
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
  private timestamp = -1;
  private pending = new Map<number, { id: number; started: number }>();
  private deadline: ReturnType<typeof setTimeout> | undefined;
  private livenessDeadline: ReturnType<typeof setTimeout> | undefined;
  private cursorShape: ImageData | null = null;
  private hotspot = { x: 0, y: 0 };
  private cursor = { x: 0, y: 0, visible: false };
  private context: CanvasRenderingContext2D;
  private cursorContext: CanvasRenderingContext2D;
  private viewportResize: ResizeObserver | undefined;
  private visibility = new AbortController();
  private displayedFrame = 0;
  private visible = true;
  private scaleMode: "native" | "fit" = "fit";
  private decodeMicroseconds = 0;
  private resizeSequence = 0;
  private resizePending = false;
  private lastProbeNonce = 0;
  private lastAcceptedResponseNonce = 0;
  private resizeDeadline: ReturnType<typeof setTimeout> | undefined;
  private quality = new DesktopQualityPolicy();
  private startedAt = performance.now();
  private firstFrameMilliseconds: number | null = null;
  private displayedCount = 0;
  private decodeTotal = 0;
  private codec = "";
  private videoBytes = 0;
  private bitrateSince = 0;
  private bitrateTimer: ReturnType<typeof setTimeout> | undefined;

  constructor(
    private ticket: DesktopTicket,
    private canvas: HTMLCanvasElement,
    private overlay: HTMLCanvasElement,
    private status: (message: string) => void,
    private ended: (hadFrame: boolean) => void,
    private displayModes: (
      modes: { width: number; height: number }[],
    ) => void = () => {},
    private bitrate: (bitsPerSecond: number) => void = () => {},
    private performanceSample: (sample: DesktopPerformance) => void = () => {},
  ) {
    const context = canvas.getContext("2d", { alpha: false });
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
      const viewport = canvas.closest(".desktop-viewport");
      if (viewport) this.viewportResize.observe(viewport);
      this.deadline = setTimeout(() => this.fail("桌面首帧超时"), 60000);
      document.addEventListener(
        "visibilitychange",
        () => this.reportDisplay(),
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
          queued: 0,
          bytes: 0,
          work: Promise.resolve(),
        };
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
          const data = event.data;
          if (
            !(data instanceof ArrayBuffer) ||
            data.byteLength > 16 * 1024 * 1024 ||
            ++channel.queued >
              (channel.purpose === 3 ? maximumPendingVideoFrames + 2 : 8) ||
            (channel.bytes += data.byteLength) > 32 * 1024 * 1024
          ) {
            this.fail("桌面接收队列超出限制");
            return;
          }
          channel.work = channel.work
            .then(async () => {
              if (!this.stopped)
                await this.receive(
                  channel,
                  wire.Envelope.decode(new Uint8Array(data)),
                );
            })
            .catch(() => this.fail("桌面数据或解码异常，会话已结束"))
            .finally(() => {
              channel.queued--;
              channel.bytes -= data.byteLength;
            });
        };
        socket.onerror = () => this.fail("桌面连接失败");
        socket.onclose = (event) =>
          this.fail(
            `桌面连接已断开（通道 ${channel.purpose}，代码 ${event.code}）`,
          );
      }
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
        (reason) => this.fail(`桌面发送拥塞或连接已断开（${reason}）`),
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
  setQuality(mode: DesktopQuality): void {
    if (this.stopped) return;
    this.quality.setMode(mode);
    this.reportDisplay();
  }
  resize(width: number, height: number): void {
    const channel = this.channels.find(
      (entry) => entry.purpose === 2 && entry.ready,
    );
    if (
      !channel ||
      this.stopped ||
      !this.ticket.writeControl ||
      !this.generation ||
      this.resizePending
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
  setVisible(visible: boolean): void {
    if (this.stopped || this.visible === visible) return;
    this.visible = visible;
    if (!visible) this.input?.release();
    this.reportDisplay();
  }
  private reportDisplay(): void {
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
          queueDepth: this.pending.size,
          background: document.hidden || !this.visible,
          preference:
            this.quality.preference === "sharp"
              ? wire.QualityFeedback.Preference.SHARP
              : this.quality.preference === "smooth"
                ? wire.QualityFeedback.Preference.SMOOTH
                : wire.QualityFeedback.Preference.BALANCED,
        },
      });
  }
  private async receive(channel: Channel, message: Message): Promise<void> {
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
        message.channelReady?.purpose !== channel.purpose
      )
        throw new Error("Desktop not ready");
      channel.ready = true;
      if (!this.displayedFrame)
        this.status("通道已配对，等待远端采集与视频首帧…");
      if (channel.purpose === 2) {
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
      if (message.body !== "cursorUpdate" || !message.cursorUpdate)
        throw new Error("Invalid cursor channel");
      const cursor = wire.CursorUpdate.create(message.cursorUpdate);
      const rgba = cursor.rgba;
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
        this.updateLocalCursor();
      }
      this.cursor = { x: cursor.x, y: cursor.y, visible: cursor.visible };
      this.drawCursor();
      return;
    }
    if (message.body === "videoConfig" && message.videoConfig) {
      const video = wire.VideoConfig.create(message.videoConfig);
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
        !/^avc1\.[0-9a-f]{6}$/i.test(video.codec)
      )
        throw new Error("Invalid video configuration");
      // Old-coordinate clicks must never cross a new video/input binding.
      // Closing the session also releases input already held by the Agent.
      const hasPendingInput = () =>
        [...this.writers.values()].some((writer) =>
          writer.hasPending((body) => !!body.inputEvent),
        );
      if (hasPendingInput()) {
        this.fail("画面已切换且输入仍在积压，请重新连接");
        return;
      }
      if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
      this.input?.dispose();
      if (this.stopped) return;
      // Disposing a held key may itself enqueue RELEASE_ALL on a congested
      // socket. Do not create a new input binding ahead of that release.
      if (hasPendingInput()) {
        this.fail("画面已切换且输入仍在积压，请重新连接");
        return;
      }
      this.input = null;
      this.decoder = null;
      this.pending.clear();
      this.generation = generationId;
      this.displayedFrame = 0;
      this.frame = 0;
      this.timestamp = -1;
      this.canvas.width = this.overlay.width = video.width;
      this.canvas.height = this.overlay.height = video.height;
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
      if (!(await VideoDecoder.isConfigSupported(configuration)).supported)
        throw new Error("Unsupported video configuration");
      if (this.stopped) return;
      const generation = this.generation;
      this.decoder = new VideoDecoder({
        error: () => this.fail("视频解码失败"),
        output: (frame) => {
          try {
            if (this.stopped || generation !== this.generation) return;
            const pending = this.pending.get(frame.timestamp);
            if (
              !pending ||
              frame.displayWidth !== this.overlay.width ||
              frame.displayHeight !== this.overlay.height
            )
              throw new Error("Unexpected decoded frame");
            this.pending.delete(frame.timestamp);
            this.context.drawImage(frame, 0, 0);
            this.canvas.dataset.frame = String(pending.id);
            this.canvas.dataset.generation = String(generation);
            const firstDisplay = !this.displayedFrame;
            this.displayedFrame = pending.id;
            if (firstDisplay) {
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
          } catch {
            this.fail("桌面显示异常");
          } finally {
            frame.close();
          }
        },
      });
      this.codec = video.codec;
      this.status("已收到视频配置，等待解码首帧…");
      this.decoder.configure(configuration);
      return;
    }
    const image = message.videoFrame
      ? wire.VideoFrame.create(message.videoFrame)
      : null;
    if (
      message.body !== "videoFrame" ||
      !image ||
      !this.decoder ||
      generationId !== this.generation ||
      integer(frameId, 1) <= this.frame ||
      baseFrameId ||
      (!this.frame && !image.keyFrame) ||
      integer(image.timestampMicroseconds) <= this.timestamp ||
      !image.data.length ||
      // Match the Agent's 16-frame display acknowledgement window. A decoder
      // stall must not reject frames that were already admitted by its sender.
      this.pending.size >= maximumPendingVideoFrames
    )
      throw new Error("Invalid video frame");
    this.recordVideoBytes(image.data.length);
    this.frame = frameId;
    this.timestamp = integer(image.timestampMicroseconds);
    this.pending.set(this.timestamp, {
      id: this.frame,
      started: performance.now(),
    });
    this.decoder.decode(
      new EncodedVideoChunk({
        type: image.keyFrame ? "key" : "delta",
        timestamp: this.timestamp,
        data: new Uint8Array(image.data),
      }),
    );
  }
  private drawCursor(): void {
    this.cursorContext.clearRect(0, 0, this.overlay.width, this.overlay.height);
    if (!this.ticket.writeControl && this.cursor.visible && this.cursorShape)
      this.cursorContext.putImageData(
        this.cursorShape,
        this.cursor.x - this.hotspot.x,
        this.cursor.y - this.hotspot.y,
      );
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
    const decodeMilliseconds = frames ? this.decodeTotal / frames : null;
    const previousQuality = this.quality.preference;
    this.quality.sample(frames, decodeMilliseconds ?? 0, this.pending.size);
    if (previousQuality !== this.quality.preference) this.reportDisplay();
    this.bitrate((bytes * 8000) / elapsed);
    this.performanceSample({
      framesPerSecond: (frames * 1000) / elapsed,
      decodeMilliseconds,
      pendingFrames: this.pending.size,
      bufferedBytes: this.channels.reduce(
        (sum, channel) => sum + channel.socket.bufferedAmount,
        0,
      ),
      receivedQueueBytes: this.channels.reduce(
        (sum, channel) => sum + channel.bytes,
        0,
      ),
      firstFrameMilliseconds: this.firstFrameMilliseconds,
      width: this.canvas.width,
      height: this.canvas.height,
      codec: this.codec,
      quality: this.quality.preference,
    });
    this.displayedCount = this.decodeTotal = 0;
    this.bitrateSince = now;
    // Publish one zero sample after traffic/display stops, then remain idle.
    if (bytes || frames)
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
      width = viewport.clientWidth;
      height = viewport.clientHeight;
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
    if (!this.ticket.writeControl || this.stopped || !this.cursorShape) return;
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
    const source = document.createElement("canvas");
    source.width = shape.width;
    source.height = shape.height;
    source.getContext("2d")?.putImageData(shape, 0, 0);
    const rendered = document.createElement("canvas");
    rendered.width = Math.max(1, Math.round(shape.width * scaleX));
    rendered.height = Math.max(1, Math.round(shape.height * scaleY));
    rendered
      .getContext("2d")
      ?.drawImage(source, 0, 0, rendered.width, rendered.height);
    const x = Math.min(rendered.width - 1, Math.round(this.hotspot.x * scaleX));
    const y = Math.min(
      rendered.height - 1,
      Math.round(this.hotspot.y * scaleY),
    );
    this.canvas.style.setProperty(
      "--desktop-cursor",
      `url("${rendered.toDataURL()}") ${x} ${y}, default`,
    );
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
      () => this.fail("远控存活确认超时"),
      60000,
    );
  }
  private fail(message: string): void {
    if (this.stopped || this.stopping) return;
    console.warn("Desktop session ended", {
      session: this.ticket.id,
      reason: message,
      channels: this.channels.map((channel) => ({
        purpose: channel.purpose,
        ready: channel.ready,
        bufferedBytes: channel.socket.bufferedAmount,
        queuedMessages: channel.queued,
        queuedBytes: channel.bytes,
      })),
    });
    const hadFrame = this.firstFrameMilliseconds !== null;
    const phase = hadFrame
      ? ""
      : !this.channels.every((channel) => channel.ready)
        ? "（通道配对阶段）"
        : !this.decoder
          ? "（远端首帧生成阶段）"
          : "（首帧接收/解码阶段）";
    this.stop();
    this.status(message + phase);
    this.ended(hadFrame);
  }
  stop(): void {
    if (this.stopped || this.stopping) return;
    this.stopping = true;
    this.stopped = true;
    for (const writer of this.writers.values()) writer.stop();
    this.writers.clear();
    this.input?.dispose();
    this.input = null;
    this.viewportResize?.disconnect();
    this.visibility.abort();
    this.canvas.style.removeProperty("--desktop-cursor");
    clearTimeout(this.deadline);
    clearTimeout(this.livenessDeadline);
    this.livenessDeadline = undefined;
    clearTimeout(this.resizeDeadline);
    clearTimeout(this.bitrateTimer);
    this.bitrateTimer = undefined;
    this.videoBytes = 0;
    for (const { socket } of this.channels) {
      socket.onopen = socket.onmessage = socket.onerror = socket.onclose = null;
      socket.close();
    }
    if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
    this.decoder = null;
    this.pending.clear();
    this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
    this.cursorContext.clearRect(0, 0, this.overlay.width, this.overlay.height);
  }
}
