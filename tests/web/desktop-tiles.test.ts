import { expect, test } from "bun:test";
import {
  DesktopTiles,
  desktopCopies,
} from "../../web/src/sessions/desktop/tiles.session";

function png(width = 128, height = 64): Uint8Array {
  const bytes = new Uint8Array(33);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0x89504e47);
  view.setUint32(4, 0x0d0a1a0a);
  view.setUint32(8, 13);
  view.setUint32(12, 0x49484452);
  view.setUint32(16, width);
  view.setUint32(20, height);
  return bytes;
}
const area = { x: 0, y: 0, width: 128, height: 64 };

test("PNG decoding overlaps with a four-image ceiling and closes late failures", async () => {
  let active = 0,
    peak = 0,
    closes = 0;
  const releases: (() => void)[] = [];
  const cache = new DesktopTiles(async () => {
    active++;
    peak = Math.max(peak, active);
    await new Promise<void>((resolve) => releases.push(resolve));
    active--;
    return {
      width: 128,
      height: 64,
      close: () => {
        closes++;
      },
    };
  });
  const patches = Array.from({ length: 8 }, (_, index) => ({
    destination: area,
    png: png(),
    cacheSlot: index + 1,
  }));
  const prepared = cache.prepare(patches, 256, 128);
  expect(active).toBe(4);
  while (closes === 0) {
    for (const release of releases.splice(0)) release();
    await new Promise((resolve) => setTimeout(resolve, 1));
    if (active === 0) break;
  }
  const batch = await prepared;
  expect(peak).toBe(4);
  expect(batch.tiles).toHaveLength(8);
  batch.dispose();
  expect(closes).toBe(8);
  let started = 0,
    failedCloses = 0;
  const failing = new DesktopTiles(async () => {
    const index = started++;
    await new Promise((resolve) => setTimeout(resolve, 1));
    if (index === 0) throw new Error("decoder failure");
    return {
      width: 128,
      height: 64,
      close: () => {
        failedCloses++;
      },
    };
  });
  await expect(failing.prepare(patches, 256, 128)).rejects.toThrow(
    "decoder failure",
  );
  expect(started).toBe(8);
  expect(failedCloses).toBe(7);
});

test("tile definitions publish atomically and reuse the same decoded image", async () => {
  let decodes = 0,
    closes = 0;
  const cache = new DesktopTiles(async () => {
    decodes++;
    return {
      width: 128,
      height: 64,
      close: () => {
        closes++;
      },
    };
  });
  const first = await cache.prepare(
    [{ destination: area, png: png(), cacheSlot: 1 }],
    256,
    128,
  );
  await expect(
    cache.prepare([{ destination: area, cacheSlot: 1 }], 256, 128),
  ).rejects.toThrow();
  first.commit();
  first.dispose();
  const reference = await cache.prepare(
    [{ destination: { ...area, x: 128 }, cacheSlot: 1 }],
    256,
    128,
  );
  expect(reference.bytes).toBe(0);
  expect(reference.tiles[0].image).toBe(first.tiles[0].image);
  reference.commit();
  reference.dispose();
  expect(decodes).toBe(1);
  expect(closes).toBe(0);
  cache.clear();
  expect(closes).toBe(1);
  await expect(
    cache.prepare([{ destination: area, cacheSlot: 1 }], 256, 128),
  ).rejects.toThrow();
});

test("failed tile batches close staged images without publishing any slot", async () => {
  let closes = 0;
  const cache = new DesktopTiles(async () => ({
    width: 128,
    height: 64,
    close: () => {
      closes++;
    },
  }));
  await expect(
    cache.prepare(
      [
        { destination: area, png: png(), cacheSlot: 2 },
        { destination: area, cacheSlot: 3 },
      ],
      256,
      128,
    ),
  ).rejects.toThrow();
  expect(closes).toBe(1);
  await expect(
    cache.prepare([{ destination: area, cacheSlot: 2 }], 256, 128),
  ).rejects.toThrow();
  const batch = await cache.prepare(
    [
      { destination: area, png: png(), cacheSlot: 2 },
      { destination: area, cacheSlot: 2 },
    ],
    256,
    128,
  );
  expect(batch.tiles[0].image).toBe(batch.tiles[1].image);
  batch.dispose();
  expect(closes).toBe(2);
});

test("PNG dimensions, cache slots and copy rectangles are bounded before decoding", async () => {
  let decodes = 0;
  const cache = new DesktopTiles(async () => {
    decodes++;
    return { width: 128, height: 64, close() {} };
  });
  for (const patch of [
    { destination: area, png: png(127) },
    { destination: area, png: png(), cacheSlot: 129 },
    { destination: { ...area, x: 200 }, png: png() },
    { destination: area },
  ])
    await expect(cache.prepare([patch], 256, 128)).rejects.toThrow();
  expect(decodes).toBe(0);
  expect(
    desktopCopies(
      [{ sourceX: 0, sourceY: 0, destination: { ...area, y: 64 } }],
      256,
      128,
    ),
  ).toHaveLength(1);
  expect(() =>
    desktopCopies([{ sourceX: 200, sourceY: 0, destination: area }], 256, 128),
  ).toThrow();
  expect(() =>
    desktopCopies(
      Array.from({ length: 33 }, () => ({ destination: area })),
      256,
      128,
    ),
  ).toThrow();
});
