// A bounded record of distinct canvas frames observed by visible rAF callbacks.
// rAF runs before compositor presentation; these are not physical display times.
export class DesktopVisibleFrameGaps {
  private readonly gaps = new Float32Array(120);
  private count = 0;
  private next = 0;
  private previous: number | null = null;

  observe(at: number): void {
    if (!Number.isFinite(at) || (this.previous !== null && at <= this.previous))
      return;
    if (this.previous !== null) {
      this.gaps[this.next] = at - this.previous;
      this.next = (this.next + 1) % this.gaps.length;
      this.count = Math.min(this.count + 1, this.gaps.length);
    }
    this.previous = at;
  }

  clear(): void {
    this.previous = null;
    this.next = this.count = 0;
  }

  summary(): {
    samples: number;
    p95Milliseconds: number | null;
    p99Milliseconds: number | null;
    gapsAtLeast100Milliseconds: number;
  } {
    const longGaps = Array.from(this.gaps.subarray(0, this.count)).filter(
      (gap) => gap >= 100,
    ).length;
    if (this.count < 20)
      return {
        samples: this.count,
        p95Milliseconds: null,
        p99Milliseconds: null,
        gapsAtLeast100Milliseconds: longGaps,
      };
    const sorted = Array.from(this.gaps.subarray(0, this.count)).sort(
      (left, right) => left - right,
    );
    return {
      samples: this.count,
      p95Milliseconds: sorted[Math.ceil(this.count * 0.95) - 1],
      p99Milliseconds: sorted[Math.ceil(this.count * 0.99) - 1],
      gapsAtLeast100Milliseconds: longGaps,
    };
  }
}
