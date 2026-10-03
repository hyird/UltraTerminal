import { expect, test } from "bun:test";
import {
  desktopCodecFamily,
  detectDesktopCodecs,
} from "../../web/src/sessions/desktop/codecs.session";
import { ultraterimal } from "../../web/src/generated/session";
const codec = ultraterimal.wire.VideoCodec;

test("codec strings retain the actual encoded family", () => {
  expect(desktopCodecFamily("avc1.42E028")).toBe(codec.AVC);
  expect(desktopCodecFamily("hev1.1.6.L153.B0")).toBe(codec.HEVC);
  expect(desktopCodecFamily("hvc1.1.6.L153.B0")).toBe(codec.HEVC);
  expect(desktopCodecFamily("av01.0.13M.08")).toBe(codec.AV1);
  expect(desktopCodecFamily("avc1.123")).toBe(codec.VIDEO_CODEC_UNSPECIFIED);
  expect(desktopCodecFamily("vp09.0")).toBe(codec.VIDEO_CODEC_UNSPECIFIED);
});

test("optional codec probe failures always preserve H264", async () => {
  const previous = Object.getOwnPropertyDescriptor(globalThis, "VideoDecoder");
  try {
    Object.defineProperty(globalThis, "VideoDecoder", {
      configurable: true,
      value: {
        isConfigSupported: async (configuration: VideoDecoderConfig) => {
          if (configuration.codec.startsWith("hvc1"))
            throw new Error("HEVC unavailable");
          return { supported: true };
        },
      },
    });
    expect(await detectDesktopCodecs()).toEqual([codec.AVC, codec.AV1]);
  } finally {
    if (previous) Object.defineProperty(globalThis, "VideoDecoder", previous);
    else Reflect.deleteProperty(globalThis, "VideoDecoder");
  }
});
