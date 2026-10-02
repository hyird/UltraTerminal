import type { ultraterimal } from "../../generated/session";

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
    if (this.count < 20)
      return {
        samples: this.count,
        p95Milliseconds: null,
        p99Milliseconds: null,
        gapsAtLeast100Milliseconds: 0,
      };
    const sorted = Array.from(this.gaps.subarray(0, this.count)).sort(
      (left, right) => left - right,
    );
    return {
      samples: this.count,
      p95Milliseconds: sorted[Math.ceil(this.count * 0.95) - 1],
      p99Milliseconds: sorted[Math.ceil(this.count * 0.99) - 1],
      gapsAtLeast100Milliseconds: sorted.filter((gap) => gap >= 100).length,
    };
  }
}

// A wire summary is emitted at most once per second and only while a visible
// stream advances. Invalid or suspended samples never disrupt video ACKs.
export function visibleFrameTiming(
  summary: ReturnType<DesktopVisibleFrameGaps["summary"]>,
  observedFramesPerSecond: number,
): ultraterimal.wire.IVisibleFrameTiming | null {
  const {
    samples,
    p95Milliseconds,
    p99Milliseconds,
    gapsAtLeast100Milliseconds,
  } = summary;
  if (
    !Number.isInteger(samples) ||
    samples < 20 ||
    samples > 120 ||
    p95Milliseconds === null ||
    p99Milliseconds === null ||
    !Number.isFinite(p95Milliseconds) ||
    !Number.isFinite(p99Milliseconds) ||
    !Number.isInteger(gapsAtLeast100Milliseconds) ||
    gapsAtLeast100Milliseconds < 0 ||
    gapsAtLeast100Milliseconds > samples ||
    !Number.isFinite(observedFramesPerSecond)
  )
    return null;
  const p95Microseconds = Math.round(p95Milliseconds * 1000);
  const p99Microseconds = Math.round(p99Milliseconds * 1000);
  const observedFpsMilli = Math.round(observedFramesPerSecond * 1000);
  if (
    p95Microseconds < 1 ||
    p95Microseconds > 30_000_000 ||
    p99Microseconds < p95Microseconds ||
    p99Microseconds > 30_000_000 ||
    observedFpsMilli < 1 ||
    observedFpsMilli > 240_000
  )
    return null;
  return {
    sampleCount: samples,
    p95Microseconds,
    p99Microseconds,
    longGapCount: gapsAtLeast100Milliseconds,
    observedFpsMilli,
  };
}
