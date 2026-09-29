// One bounded writer per socket. Only adjacent replaceable state may coalesce;
// key/button/wheel/release messages remain ordered and are never discarded.
export class DesktopOutbound<T> {
  private pending: { body: T; key: string; since: number }[] = [];
  private timer: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;
  constructor(
    private socket: WebSocket,
    private encode: (body: T) => Uint8Array<ArrayBuffer>,
    private failed: (reason: string) => void,
  ) {}

  send(body: T, key = ""): void {
    if (this.stopped) return;
    const previous = this.pending.at(-1);
    if (key && previous?.key === key) previous.body = body;
    else {
      if (this.pending.length >= 128) return this.fail("queue_full");
      this.pending.push({ body, key, since: performance.now() });
    }
    this.flush();
  }

  hasPending(predicate: (body: T) => boolean): boolean {
    return this.pending.some((entry) => predicate(entry.body));
  }

  private flush(): void {
    clearTimeout(this.timer);
    this.timer = undefined;
    if (this.stopped) return;
    try {
      if (this.socket.readyState !== WebSocket.OPEN)
        return this.fail("socket_not_open");
      while (this.pending.length) {
        const next = this.pending[0];
        // Never replay stale control input after a prolonged outage.
        if (performance.now() - next.since >= 1000)
          return this.fail("input_expired");
        if (this.socket.bufferedAmount > 16384) {
          this.timer = setTimeout(() => this.flush(), 25);
          return;
        }
        this.socket.send(this.encode(next.body));
        this.pending.shift();
      }
    } catch {
      this.fail("send_error");
    }
  }

  private fail(reason: string): void {
    this.stop();
    this.failed(reason);
  }

  stop(): void {
    this.stopped = true;
    clearTimeout(this.timer);
    this.timer = undefined;
    this.pending = [];
  }
}
