import { expect, test } from "bun:test";
import { ultraterimal } from "../../web/src/generated/session";
import {
  DesktopVisibleFrameGaps,
  visibleFrameTiming,
} from "../../web/src/sessions/desktop/visible_frames.session";

test("reports rAF gap quantiles only after enough distinct visible frames", () => {
  const gaps = new DesktopVisibleFrameGaps();
  gaps.observe(0);
  for (let frame = 1; frame <= 19; frame++) gaps.observe(frame * 33);
  expect(gaps.summary()).toEqual({
    samples: 19,
    p95Milliseconds: null,
    p99Milliseconds: null,
    gapsAtLeast100Milliseconds: 0,
  });
  gaps.observe(20 * 33);
  gaps.observe(20 * 33 + 120);
  expect(gaps.summary()).toEqual({
    samples: 21,
    p95Milliseconds: 33,
    p99Milliseconds: 120,
    gapsAtLeast100Milliseconds: 1,
  });
  const timing = visibleFrameTiming(gaps.summary(), 27.5);
  expect(timing).toEqual({
    sampleCount: 21,
    p95Microseconds: 33000,
    p99Microseconds: 120000,
    longGapCount: 1,
    observedFpsMilli: 27500,
  });
  const bytes = ultraterimal.wire.QualityFeedback.encode({
    displayedFrame: 21,
    visibleFrameTiming: timing,
  }).finish();
  expect(
    ultraterimal.wire.QualityFeedback.decode(bytes).visibleFrameTiming
      ?.p95Microseconds,
  ).toBe(33000);
});

test("bounds the visible sample window and resets on hide or generation change", () => {
  const gaps = new DesktopVisibleFrameGaps();
  let at = 0;
  gaps.observe(at);
  for (let frame = 1; frame <= 120; frame++) gaps.observe((at += 33));
  for (let frame = 1; frame <= 7; frame++) gaps.observe((at += 120));
  expect(gaps.summary()).toEqual({
    samples: 120,
    p95Milliseconds: 120,
    p99Milliseconds: 120,
    gapsAtLeast100Milliseconds: 7,
  });
  gaps.clear();
  gaps.observe(at + 10000);
  gaps.observe(at + 10033);
  expect(gaps.summary()).toEqual({
    samples: 1,
    p95Milliseconds: null,
    p99Milliseconds: null,
    gapsAtLeast100Milliseconds: 0,
  });
});

test("does not mistake invalid or duplicate callback timestamps for frame gaps", () => {
  const gaps = new DesktopVisibleFrameGaps();
  gaps.observe(100);
  gaps.observe(Number.NaN);
  gaps.observe(100);
  gaps.observe(90);
  gaps.observe(133);
  expect(gaps.summary().samples).toBe(1);
  gaps.clear();
  expect(gaps.summary().samples).toBe(0);
});

test("does not send absent, suspended or invalid rAF measurements", () => {
  const gaps = new DesktopVisibleFrameGaps();
  gaps.observe(0);
  for (let frame = 1; frame <= 19; frame++) gaps.observe(frame * 33);
  expect(visibleFrameTiming(gaps.summary(), 27.5)).toBeNull();
  const summary = {
    samples: 120,
    p95Milliseconds: 33,
    p99Milliseconds: 120,
    gapsAtLeast100Milliseconds: 1,
  };
  expect(visibleFrameTiming({ ...summary, samples: 121 }, 27.5)).toBeNull();
  expect(
    visibleFrameTiming({ ...summary, p99Milliseconds: 30_001 }, 27.5),
  ).toBeNull();
  expect(
    visibleFrameTiming({ ...summary, p99Milliseconds: 30 }, 27.5),
  ).toBeNull();
  expect(
    visibleFrameTiming({ ...summary, gapsAtLeast100Milliseconds: 121 }, 27.5),
  ).toBeNull();
  expect(visibleFrameTiming(summary, 0)).toBeNull();
  expect(visibleFrameTiming(summary, 241)).toBeNull();
});
