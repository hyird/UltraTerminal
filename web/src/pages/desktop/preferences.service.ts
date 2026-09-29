import type { DesktopQuality } from "../../sessions/desktop/quality.session";

export interface DesktopPreferences {
  scale: "fit" | "native";
  quality: DesktopQuality;
  resolution: { width: number; height: number } | null;
}

function storageKey(userId: string, deviceId: string) {
  return `ultraterimal.desktop.v1:${encodeURIComponent(userId)}:${encodeURIComponent(deviceId)}`;
}

export function loadDesktopPreferences(
  userId: string,
  deviceId: string,
): DesktopPreferences {
  const defaults: DesktopPreferences = {
    scale: "fit",
    quality: "auto",
    resolution: null,
  };
  try {
    const value: unknown = JSON.parse(
      localStorage.getItem(storageKey(userId, deviceId)) ?? "null",
    );
    if (!value || typeof value !== "object") return defaults;
    if ("scale" in value && value.scale === "native") defaults.scale = "native";
    if (
      "quality" in value &&
      (value.quality === "smooth" || value.quality === "sharp")
    )
      defaults.quality = value.quality;
    if (
      "resolution" in value &&
      value.resolution &&
      typeof value.resolution === "object"
    ) {
      const mode = value.resolution;
      if (
        "width" in mode &&
        "height" in mode &&
        typeof mode.width === "number" &&
        typeof mode.height === "number" &&
        Number.isInteger(mode.width) &&
        Number.isInteger(mode.height) &&
        mode.width >= 16 &&
        mode.width <= 4096 &&
        mode.height >= 16 &&
        mode.height <= 2160 &&
        mode.width % 2 === 0 &&
        mode.height % 2 === 0
      )
        defaults.resolution = { width: mode.width, height: mode.height };
    }
  } catch {
    // Invalid or unavailable browser storage must not prevent a connection.
  }
  return defaults;
}

export function saveDesktopPreferences(
  userId: string,
  deviceId: string,
  value: DesktopPreferences,
): void {
  try {
    localStorage.setItem(storageKey(userId, deviceId), JSON.stringify(value));
  } catch {
    // Keep the current session usable when browser storage is unavailable.
  }
}
