import type { ultraterimal } from "../../generated/session";

type Patch = ultraterimal.wire.IPatchRect;
export interface DesktopDecodedTile {
  x: number;
  y: number;
  image: ImageBitmap;
}
export interface DesktopTileBatch {
  tiles: DesktopDecodedTile[];
  bytes: number;
  commit(): void;
  dispose(): void;
}
const natural = (value: number | null | undefined, minimum = 0): number => {
  if (!Number.isSafeInteger(value) || (value ?? -1) < minimum)
    throw new Error("Invalid desktop tile integer");
  return value ?? 0;
};
export function desktopCopies(
  copies: ultraterimal.wire.ICopyRect[],
  width: number,
  height: number,
) {
  if (copies.length > 32) throw new Error("Desktop copy count exceeded");
  let pixels = 0;
  const result = copies.map((copy) => {
    if (!copy.destination) throw new Error("Missing desktop copy area");
    const x = natural(copy.destination.x ?? 0),
      y = natural(copy.destination.y ?? 0),
      w = natural(copy.destination.width, 1),
      h = natural(copy.destination.height, 1),
      sourceX = natural(copy.sourceX ?? 0),
      sourceY = natural(copy.sourceY ?? 0);
    if (
      x + w > width ||
      y + h > height ||
      sourceX + w > width ||
      sourceY + h > height
    )
      throw new Error("Desktop copy outside canvas");
    pixels += w * h;
    return { x, y, width: w, height: h, sourceX, sourceY };
  });
  if (pixels > 33554432) throw new Error("Desktop copy budget exceeded");
  return result;
}

// At most 128 decoded tiles (4 MiB). Owned by one desktop session and retired
// on generation changes. A failed batch never publishes cache definitions.
export class DesktopTiles {
  private slots = new Map<number, ImageBitmap>();
  constructor(
    private decode: (png: Uint8Array) => Promise<ImageBitmap> = (png) =>
      createImageBitmap(new Blob([new Uint8Array(png)], { type: "image/png" })),
  ) {}

  clear(): void {
    for (const image of this.slots.values()) image.close();
    this.slots.clear();
  }

  async prepare(
    patches: Patch[],
    canvasWidth: number,
    canvasHeight: number,
  ): Promise<DesktopTileBatch> {
    if (patches.length > 32) throw new Error("Desktop patch count exceeded");
    let bytes = 0,
      pixels = 0;
    // Check the entire byte/pixel budget before decoding any image.
    const areas = patches.map((patch) => {
      if (!patch.destination) throw new Error("Missing desktop patch area");
      const x = natural(patch.destination.x ?? 0),
        y = natural(patch.destination.y ?? 0),
        width = natural(patch.destination.width, 1),
        height = natural(patch.destination.height, 1),
        slot = natural(patch.cacheSlot ?? 0),
        png = patch.png ?? new Uint8Array();
      if (
        width > 128 ||
        height > 64 ||
        slot > 128 ||
        x + width > canvasWidth ||
        y + height > canvasHeight ||
        (!png.length && !slot)
      )
        throw new Error("Invalid desktop tile geometry");
      if (png.length) {
        if (png.length < 33 || png.length > 65536)
          throw new Error("Invalid desktop PNG size");
        const header = new DataView(png.buffer, png.byteOffset, png.byteLength);
        if (
          header.getUint32(0) !== 0x89504e47 ||
          header.getUint32(4) !== 0x0d0a1a0a ||
          header.getUint32(8) !== 13 ||
          header.getUint32(12) !== 0x49484452 ||
          header.getUint32(16) !== width ||
          header.getUint32(20) !== height
        )
          throw new Error("Invalid desktop PNG header");
      }
      bytes += png.length;
      pixels += width * height;
      return { x, y, width, height, slot, png };
    });
    if (bytes > 256 * 1024 || pixels > 256 * 1024)
      throw new Error("Desktop tile budget exceeded");
    const definitions = new Map<number, ImageBitmap>();
    const owned = new Set<ImageBitmap>();
    const decoded = new Map<number, ImageBitmap>();
    const tiles: DesktopDecodedTile[] = [];
    try {
      let next = 0;
      // Wait for every bounded worker before cleanup; late completions own images.
      const workers = await Promise.allSettled(
        Array.from({ length: Math.min(4, areas.length) }, async () => {
          while (next < areas.length) {
            const index = next++;
            const area = areas[index];
            if (!area.png.length) continue;
            const image = await this.decode(area.png);
            owned.add(image);
            decoded.set(index, image);
          }
        }),
      );
      for (const worker of workers)
        if (worker.status === "rejected") throw worker.reason;
      for (const [index, area] of areas.entries()) {
        const image = area.png.length
          ? decoded.get(index)
          : (definitions.get(area.slot) ?? this.slots.get(area.slot));
        if (
          !image ||
          image.width !== area.width ||
          image.height !== area.height
        )
          throw new Error("Unknown or mismatched desktop cache slot");
        if (area.slot && area.png.length) definitions.set(area.slot, image);
        tiles.push({ x: area.x, y: area.y, image });
      }
    } catch (error) {
      for (const image of owned) image.close();
      throw error;
    }
    let committed = false;
    return {
      tiles,
      bytes,
      commit: () => {
        if (committed) throw new Error("Desktop tile batch committed twice");
        for (const [slot, image] of definitions) {
          this.slots.get(slot)?.close();
          this.slots.set(slot, image);
        }
        committed = true;
      },
      dispose: () => {
        const retained = new Set(committed ? this.slots.values() : []);
        for (const image of owned) if (!retained.has(image)) image.close();
        owned.clear();
      },
    };
  }
}
