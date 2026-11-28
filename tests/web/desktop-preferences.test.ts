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
const defaults = { scale: "fit", resolution: "auto", muted: false };

test("desktop preferences persist scale, resolution and muted state for each user and device", () => {
  const storage = storageFixture();
  try {
    for (const scale of ["native", "fit"] as const) {
      for (const muted of [false, true]) {
        storage.entries.set(
          key,
          JSON.stringify({
            scale,
            unsupported: true,
            resolution: { width: 1920, height: 1080 },
            muted,
          }),
        );
        const preferences = loadDesktopPreferences("user", "device");
        expect(preferences).toEqual({
          scale,
          resolution: { width: 1920, height: 1080 },
          muted,
        });
        expect(JSON.parse(storage.entries.get(key) ?? "null")).toEqual(
          preferences,
        );
        saveDesktopPreferences("user", "device", preferences);
        expect(JSON.parse(storage.entries.get(key) ?? "null")).toEqual(
          preferences,
        );
      }
    }
    expect(loadDesktopPreferences("other-user", "device")).toEqual(defaults);
    expect(loadDesktopPreferences("user", "other-device")).toEqual(defaults);
  } finally {
    storage.restore();
  }
});

test("desktop preferences tolerate invalid or unavailable storage and still validate fields", () => {
  const storage = storageFixture();
  try {
    expect(loadDesktopPreferences("user", "device")).toEqual(defaults);
    for (const raw of [
      "invalid JSON",
      "null",
      "42",
      JSON.stringify({
        scale: "invalid",
        resolution: { width: 1919, height: 1080 },
        muted: "yes",
      }),
      JSON.stringify({
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
        muted: true,
      }),
    ).not.toThrow();
  } finally {
    storage.restore();
  }
});
