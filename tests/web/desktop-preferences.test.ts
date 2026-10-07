import { expect, test } from "bun:test";
import {
  loadDesktopPreferences,
  saveDesktopPreferences,
} from "../../web/src/pages/desktop/preferences.service";

function storageFixture() {
  const previous = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  const entries = new Map<string, string>();
  let unavailable = false;
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true,
    value: {
      getItem(key: string) {
        if (unavailable) throw new Error("Storage unavailable");
        return entries.get(key) ?? null;
      },
      setItem(key: string, value: string) {
        if (unavailable) throw new Error("Storage unavailable");
        entries.set(key, value);
      },
    },
  });
  return {
    entries,
    block() {
      unavailable = true;
    },
    restore() {
      if (previous) Object.defineProperty(globalThis, "localStorage", previous);
      else Reflect.deleteProperty(globalThis, "localStorage");
    },
  };
}

const key = "ultraterimal.desktop.v1:user:device";
const defaults = { scale: "fit", resolution: null };

test("desktop preferences ignore every legacy quality while preserving scale and resolution", () => {
  const storage = storageFixture();
  try {
    for (const quality of ["auto", "balanced", "smooth", "sharp", null, {}]) {
      storage.entries.set(
        key,
        JSON.stringify({
          scale: "native",
          quality,
          resolution: { width: 1920, height: 1080 },
        }),
      );
      const preferences = loadDesktopPreferences("user", "device");
      expect(preferences).toEqual({
        scale: "native",
        resolution: { width: 1920, height: 1080 },
      });
      saveDesktopPreferences("user", "device", preferences);
      expect(JSON.parse(storage.entries.get(key) ?? "null")).toEqual(
        preferences,
      );
    }
    // The writer only persists supported fields, even with a legacy object.
    const legacy = {
      ...loadDesktopPreferences("user", "device"),
      quality: "smooth",
    };
    saveDesktopPreferences("user", "device", legacy);
    expect(storage.entries.get(key)).not.toContain("quality");
    expect(loadDesktopPreferences("other-user", "device")).toEqual(defaults);
    expect(loadDesktopPreferences("user", "other-device")).toEqual(defaults);
  } finally {
    storage.restore();
  }
});

test("desktop preferences tolerate invalid or unavailable storage and still validate resolution", () => {
  const storage = storageFixture();
  try {
    expect(loadDesktopPreferences("user", "device")).toEqual(defaults);
    for (const raw of [
      "invalid JSON",
      "null",
      "42",
      JSON.stringify({
        scale: "invalid",
        quality: "smooth",
        resolution: { width: 1919, height: 1080 },
      }),
      JSON.stringify({
        quality: "sharp",
        resolution: { width: 8192, height: 2160 },
      }),
    ]) {
      storage.entries.set(key, raw);
      expect(loadDesktopPreferences("user", "device")).toEqual(defaults);
    }
    storage.block();
    expect(loadDesktopPreferences("user", "device")).toEqual(defaults);
    expect(() =>
      saveDesktopPreferences("user", "device", {
        scale: "native",
        resolution: null,
      }),
    ).not.toThrow();
  } finally {
    storage.restore();
  }
});
