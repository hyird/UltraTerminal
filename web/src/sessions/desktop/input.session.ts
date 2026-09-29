import { ultraterimal } from "../../generated/session";
import { virtualKey } from "./input.protocol";

type Input = ultraterimal.wire.IInputEvent;
const Kind = ultraterimal.wire.InputEvent.Kind;
const buttonBits = [1, 4, 2, 8, 16];

// Owns DOM listeners and locally held input for one writable desktop binding.
export class DesktopInput {
  private listeners = new AbortController();
  private heldKeys = new Set<number>();
  private inferredShift = false;
  private buttons = 0;
  private pointer: number | null = null;
  private move: { x: number; y: number } | null = null;
  private moveTimer = 0;
  private animation = 0;
  private wheelRemainder = [0, 0];
  private disposed = false;
  constructor(
    private canvas: HTMLCanvasElement,
    private send: (event: Input) => void,
    private source: { width: number; height: number } = canvas,
    private congested: () => boolean = () => false,
  ) {
    const options = { signal: this.listeners.signal };
    canvas.addEventListener(
      "keydown",
      (event) => this.key(event, true),
      options,
    );
    canvas.addEventListener(
      "keyup",
      (event) => this.key(event, false),
      options,
    );
    canvas.addEventListener(
      "pointerdown",
      (event) => this.point(event, true),
      options,
    );
    canvas.addEventListener(
      "pointermove",
      (event) => this.point(event, false),
      options,
    );
    canvas.addEventListener(
      "pointerup",
      (event) => this.point(event, false),
      options,
    );
    canvas.addEventListener("pointercancel", () => this.release(), options);
    canvas.addEventListener(
      "lostpointercapture",
      () => {
        if (this.pointer !== null) this.release();
      },
      options,
    );
    canvas.addEventListener("blur", () => this.release(), options);
    canvas.addEventListener(
      "contextmenu",
      (event) => event.preventDefault(),
      options,
    );
    canvas.addEventListener(
      "auxclick",
      (event) => event.preventDefault(),
      options,
    );
    canvas.addEventListener("wheel", (event) => this.wheel(event), {
      ...options,
      passive: false,
    });
    window.addEventListener("blur", () => this.release(), options);
    window.addEventListener("pagehide", () => this.release(), options);
    document.addEventListener(
      "visibilitychange",
      () => {
        if (document.hidden) this.release();
      },
      options,
    );
    canvas.tabIndex = 0;
    canvas.dataset.control = "ready";
  }
  private focused(): boolean {
    return (
      !this.disposed &&
      document.activeElement === this.canvas &&
      !document.hidden
    );
  }
  shortcut(keys: readonly string[]): boolean {
    if (this.disposed || document.hidden || keys.length < 1 || keys.length > 4)
      return false;
    const codes = keys.map(virtualKey);
    if (codes.some((code) => code === undefined)) return false;
    this.release();
    this.canvas.focus({ preventScroll: true });
    try {
      for (const code of codes) {
        if (code === undefined) continue;
        this.heldKeys.add(code);
        this.send({ kind: Kind.KEY, code, pressed: true });
      }
    } finally {
      for (const code of [...this.heldKeys].reverse()) {
        this.send({ kind: Kind.KEY, code, pressed: false });
        this.heldKeys.delete(code);
      }
    }
    return true;
  }
  secureAttention(): boolean {
    if (this.disposed || document.hidden) return false;
    this.release();
    this.canvas.focus({ preventScroll: true });
    this.send({ kind: Kind.SECURE_ATTENTION });
    return true;
  }
  private key(event: KeyboardEvent, pressed: boolean): void {
    if (!this.focused()) return;
    const code =
      virtualKey(event.code) ??
      (event.key === "Shift"
        ? event.location === 2
          ? 0xa1
          : 0xa0
        : undefined);
    if (code === undefined) return;
    const shift = code === 0xa0 || code === 0xa1;
    const modifier =
      (code >= 0xa0 && code <= 0xa5) || code === 0x5b || code === 0x5c;
    // Composition owns text, not physical modifiers or already-owned keyups.
    if (event.isComposing && pressed && !modifier) return;
    event.preventDefault();
    if (pressed && event.repeat && !this.heldKeys.has(code)) return;
    if (
      !pressed &&
      !this.heldKeys.has(code) &&
      (!shift || (!this.heldKeys.has(0xa0) && !this.heldKeys.has(0xa1)))
    )
      return;
    this.flushMove();
    if (shift) {
      if (this.inferredShift) {
        if (!pressed) {
          if (!event.shiftKey) this.syncShift(false);
          return;
        }
        if (code !== 0xa0) this.sendKey(0xa0, false);
        this.inferredShift = false;
      }
      if (pressed || this.heldKeys.has(code)) this.sendKey(code, pressed);
      if (!pressed) this.syncShift(event.shiftKey);
    } else {
      this.syncShift(event.shiftKey);
      this.sendKey(code, pressed);
    }
  }
  private sendKey(code: number, pressed: boolean): void {
    if (pressed) this.heldKeys.add(code);
    else this.heldKeys.delete(code);
    this.send({ kind: Kind.KEY, code, pressed });
  }
  private syncShift(pressed: boolean): void {
    if (pressed) {
      if (!this.heldKeys.has(0xa0) && !this.heldKeys.has(0xa1)) {
        // A chord carries Shift state even when its initial keydown was lost
        // or happened before focus. Without a side, use the protocol's left
        // Shift; a subsequent identified Shift event takes over ownership.
        this.inferredShift = true;
        this.sendKey(0xa0, true);
      }
    } else {
      for (const code of [0xa0, 0xa1]) {
        if (this.heldKeys.has(code)) this.sendKey(code, false);
      }
      this.inferredShift = false;
    }
  }
  private point(event: PointerEvent, down: boolean): void {
    if (this.disposed || event.pointerType !== "mouse") return;
    if (down) this.canvas.focus({ preventScroll: true });
    if (
      !this.focused() ||
      (this.pointer !== null && event.pointerId !== this.pointer)
    )
      return;
    const bounds = this.canvas.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    event.preventDefault();
    this.move = {
      x: Math.max(
        0,
        Math.min(
          this.source.width - 1,
          Math.floor(
            ((event.clientX - bounds.left) * this.source.width) / bounds.width,
          ),
        ),
      ),
      y: Math.max(
        0,
        Math.min(
          this.source.height - 1,
          Math.floor(
            ((event.clientY - bounds.top) * this.source.height) / bounds.height,
          ),
        ),
      ),
    };
    // Pointer events report chorded mouse button transitions through `buttons`.
    // Never adopt a drag started outside this canvas.
    const next = down || this.pointer !== null ? event.buttons & 31 : 0;
    if (down && this.pointer === null) {
      this.pointer = event.pointerId;
      this.canvas.setPointerCapture(event.pointerId);
    }
    if (next !== this.buttons) {
      this.flushMove();
      const previous = this.buttons;
      this.buttons = next;
      buttonBits.forEach((bit, code) => {
        if ((previous & bit) !== (next & bit))
          this.send({ kind: Kind.BUTTON, code, pressed: !!(next & bit) });
      });
    } else if (!this.moveTimer && !this.animation) {
      // Healthy links follow the next display tick; only a backed-up uplink
      // coalesces more movement. Discrete input still flushes immediately.
      if (this.congested())
        this.moveTimer = window.setTimeout(() => this.flushMove(), 16);
      else this.animation = requestAnimationFrame(() => this.flushMove());
    }
    if (!next && this.pointer !== null) {
      const pointer = this.pointer;
      this.pointer = null;
      if (this.canvas.hasPointerCapture(pointer))
        this.canvas.releasePointerCapture(pointer);
    }
  }
  private flushMove(): void {
    if (this.animation) cancelAnimationFrame(this.animation);
    this.animation = 0;
    if (this.moveTimer) clearTimeout(this.moveTimer);
    this.moveTimer = 0;
    const move = this.move;
    this.move = null;
    if (move && !this.disposed) this.send({ kind: Kind.POINTER_MOVE, ...move });
  }
  private wheel(event: WheelEvent): void {
    if (!this.focused()) return;
    event.preventDefault();
    this.flushMove();
    const scale =
      event.deltaMode === WheelEvent.DOM_DELTA_LINE
        ? 40
        : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
          ? 120
          : 1;
    [-event.deltaY, event.deltaX].forEach((delta, code) => {
      const accumulated =
        (this.wheelRemainder[code] ?? 0) +
        Math.max(-12000, Math.min(12000, delta * scale));
      const wheelDelta = Math.trunc(accumulated);
      this.wheelRemainder[code] = accumulated - wheelDelta;
      if (wheelDelta) this.send({ kind: Kind.WHEEL, code, wheelDelta });
    });
  }
  release(): void {
    if (this.disposed) return;
    const held = this.heldKeys.size > 0 || this.buttons !== 0;
    this.heldKeys.clear();
    this.inferredShift = false;
    this.buttons = 0;
    this.move = null;
    this.wheelRemainder = [0, 0];
    if (this.animation) cancelAnimationFrame(this.animation);
    this.animation = 0;
    if (this.moveTimer) clearTimeout(this.moveTimer);
    this.moveTimer = 0;
    const pointer = this.pointer;
    this.pointer = null;
    if (held) this.send({ kind: Kind.RELEASE_ALL });
    if (pointer !== null && this.canvas.hasPointerCapture(pointer))
      this.canvas.releasePointerCapture(pointer);
  }
  dispose(): void {
    if (this.disposed) return;
    this.release();
    this.disposed = true;
    this.listeners.abort();
    this.canvas.tabIndex = -1;
    delete this.canvas.dataset.control;
  }
}
