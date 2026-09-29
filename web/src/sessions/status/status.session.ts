import { authEvents, record } from "../../lib/http";

export class StatusSession {
  private socket: WebSocket | undefined;
  private retry: ReturnType<typeof setTimeout> | undefined;
  private heartbeat: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;
  private failures = 0;
  private readonly expired = () => this.close();
  constructor(
    private readonly update: (event: Record<string, unknown>) => void,
    private readonly failed: (message: string) => void,
    private readonly scope: "personal" | "administration" = "personal",
  ) {
    authEvents.addEventListener("expired", this.expired);
    this.connect();
  }
  private connect() {
    if (this.stopped) return;
    let socket: WebSocket;
    try {
      socket = new WebSocket(
        `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/api/events${this.scope === "administration" ? "?scope=administration" : ""}`,
      );
    } catch {
      this.scheduleRetry();
      return;
    }
    this.socket = socket;
    const heartbeat = () => {
      clearTimeout(this.heartbeat);
      this.heartbeat = setTimeout(() => socket.close(), 45000);
    };
    heartbeat();
    socket.onopen = heartbeat;
    socket.onmessage = (message: MessageEvent<unknown>) => {
      try {
        if (typeof message.data !== "string" || message.data.length > 1048576)
          throw new Error("状态消息格式错误");
        const event = record(JSON.parse(message.data));
        if (event.type === "ping") {
          socket.send('{"type":"pong"}');
          heartbeat();
          return;
        }
        this.update(event);
        this.failures = 0;
        heartbeat();
      } catch {
        this.failed("状态消息格式错误，请重新加载页面");
        this.close();
      }
    };
    socket.onclose = () => {
      clearTimeout(this.heartbeat);
      if (this.stopped) return;
      this.scheduleRetry();
    };
  }
  private scheduleRetry() {
    if (this.stopped) return;
    clearTimeout(this.retry);
    this.failures = Math.min(this.failures + 1, 6);
    this.failed("状态连接已断开，正在自动重连…");
    this.retry = setTimeout(
      () => this.connect(),
      Math.min(30000, 1000 * 2 ** (this.failures - 1)),
    );
  }
  close() {
    this.stopped = true;
    authEvents.removeEventListener("expired", this.expired);
    clearTimeout(this.retry);
    clearTimeout(this.heartbeat);
    if (this.socket) {
      this.socket.onopen = null;
      this.socket.onmessage = null;
      this.socket.onclose = null;
      this.socket.close();
    }
  }
}
