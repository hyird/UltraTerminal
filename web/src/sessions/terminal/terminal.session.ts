import { ultraterimal } from "../../generated/session";
import type { TerminalCallbacks, TerminalTicket } from "./terminal.types";

const wire = ultraterimal.wire;
const kMaxTerminalChunkBytes = 64 * 1024;

export class TerminalSession {
  private socket: WebSocket | null = null;
  private sequence = 0;
  private ready = false;
  private closed = false;
  private exited = false;
  private columns = 80;
  private rows = 24;
  private readonly encoder = new TextEncoder();

  constructor(
    private readonly ticket: TerminalTicket,
    private readonly callbacks: TerminalCallbacks,
    initialSize?: { columns: number; rows: number },
  ) {
    if (initialSize && initialSize.columns > 0 && initialSize.rows > 0) {
      this.columns = initialSize.columns;
      this.rows = initialSize.rows;
    }
    this.connect();
  }

  private connect(): void {
    this.callbacks.onStatus("connecting", "正在连接终端会话…");
    const url = new URL("/api/sessions/terminal", location.href);
    url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(url);
    socket.binaryType = "arraybuffer";
    this.socket = socket;

    socket.onopen = () => {
      if (this.closed) return;
      this.send({
        channelOpen: {
          purpose: wire.ChannelPurpose.SSH_STREAM,
          ticket: this.encoder.encode(this.ticket.channel.ticket),
        },
      });
    };

    socket.onmessage = (event: MessageEvent<unknown>) => {
      if (this.closed) return;
      if (!(event.data instanceof ArrayBuffer)) {
        this.fail("终端消息格式无效");
        return;
      }
      try {
        const message = wire.Envelope.decode(new Uint8Array(event.data));
        this.handleMessage(message);
      } catch {
        this.fail("终端数据解析失败");
      }
    };

    socket.onerror = () => {
      if (this.closed) return;
      this.fail("终端连接异常中断");
    };

    socket.onclose = (event) => {
      if (this.closed) return;
      this.ready = false;
      this.closed = true;
      if (!this.exited) {
        this.callbacks.onStatus(
          "disconnected",
          `终端连接已断开（代码 ${event.code}）`,
        );
      }
    };
  }

  private handleMessage(message: ultraterimal.wire.Envelope): void {
    if (message.channelReady) {
      this.ready = true;
      this.callbacks.onStatus("connected", "已连接");
      this.send({
        terminalResize: {
          columns: this.columns,
          rows: this.rows,
        },
      });
      return;
    }
    if (message.terminalData?.data) {
      this.callbacks.onData(message.terminalData.data);
      return;
    }
    if (message.terminalExit) {
      this.exited = true;
      this.ready = false;
      const code = message.terminalExit.exitCode ?? 0;
      this.callbacks.onStatus("exited", `终端进程已退出（退出码 ${code}）`);
      this.callbacks.onExit(code);
      this.close(false);
      return;
    }
    if (message.sessionError) {
      this.fail(message.sessionError.message || "终端会话发生错误");
    }
  }

  private send(body: ultraterimal.wire.IEnvelope): void {
    if (
      this.closed ||
      !this.socket ||
      this.socket.readyState !== WebSocket.OPEN
    ) {
      return;
    }
    const payload = new Uint8Array(
      wire.Envelope.encode({
        ...body,
        protocolVersion: 1,
        sessionId: this.ticket.id,
        channelId: this.ticket.channel.id,
        sequence: ++this.sequence,
      }).finish(),
    );
    this.socket.send(payload);
  }

  private fail(reason: string): void {
    if (this.closed) return;
    this.ready = false;
    this.callbacks.onError(reason);
    this.callbacks.onStatus("disconnected", reason);
    this.close(false);
  }

  sendInput(data: string | Uint8Array): void {
    if (this.closed || !this.ready) return;
    const bytes = typeof data === "string" ? this.encoder.encode(data) : data;
    for (
      let offset = 0;
      offset < bytes.length;
      offset += kMaxTerminalChunkBytes
    ) {
      const slice = bytes.subarray(
        offset,
        Math.min(bytes.length, offset + kMaxTerminalChunkBytes),
      );
      this.send({
        terminalData: {
          data: slice,
        },
      });
    }
  }

  resize(columns: number, rows: number): void {
    if (
      !Number.isInteger(columns) ||
      !Number.isInteger(rows) ||
      columns < 1 ||
      rows < 1 ||
      columns > 1000 ||
      rows > 500
    ) {
      return;
    }
    this.columns = columns;
    this.rows = rows;
    if (this.closed || !this.ready) return;
    this.send({
      terminalResize: {
        columns,
        rows,
      },
    });
  }

  close(notify = true): void {
    if (this.closed && !this.socket) return;
    this.closed = true;
    this.ready = false;
    if (notify && !this.exited) {
      this.callbacks.onStatus("disconnected", "终端会话已关闭");
    }
    if (this.socket) {
      const socket = this.socket;
      this.socket = null;
      socket.onopen = null;
      socket.onmessage = null;
      socket.onerror = null;
      socket.onclose = null;
      try {
        socket.close();
      } catch {
        // Ignore close errors during teardown.
      }
    }
  }
}
