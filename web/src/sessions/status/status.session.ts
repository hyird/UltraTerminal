import { record } from "../../lib/http";

export class StatusSession {
  private socket: WebSocket | undefined;
  private retry: ReturnType<typeof setTimeout> | undefined;
  private heartbeat: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;
  private failures = 0;
  constructor(
    private readonly update: (event: Record<string, unknown>) => void,
    private readonly failed: (message: string) => void,
    private readonly scope: "personal" | "administration" = "personal",
  ) {
    this.connect();
  }
  private connect() {
    if (this.stopped) return;
    const socket = new WebSocket(
      `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/api/events${this.scope === "administration" ? "?scope=administration" : ""}`,
    );
    this.socket = socket;
    const heartbeat = () => {
      clearTimeout(this.heartbeat);
      this.heartbeat = setTimeout(() => socket.close(), 45000);
    };
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
      if (++this.failures > 5) {
        this.failed("状态连接已断开，请检查登录状态并重新加载页面");
        return;
      }
      this.retry = setTimeout(
        () => this.connect(),
        Math.min(30000, 1000 * 2 ** (this.failures - 1)),
      );
    };
  }
  close() {
    this.stopped = true;
    clearTimeout(this.retry);
    clearTimeout(this.heartbeat);
    if (this.socket) {
      this.socket.onmessage = null;
      this.socket.onclose = null;
      this.socket.close();
    }
  }
}
