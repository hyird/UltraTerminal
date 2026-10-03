import { ultraterimal } from "../../generated/session";

const wire = ultraterimal.wire;
export function desktopCodecFamily(
  codec: string,
): ultraterimal.wire.VideoCodec {
  if (/^avc1\.[0-9a-f]{6}$/i.test(codec)) return wire.VideoCodec.AVC;
  if (
    /^(hev1|hvc1)\.[A-C]?[0-9]+\.[0-9a-f]+\.[LH][0-9]+(?:\.[0-9a-f]+)*$/i.test(
      codec,
    )
  )
    return wire.VideoCodec.HEVC;
  if (/^av01\.[012]\.[0-9]{2}[MH]\.(08|10|12)$/i.test(codec))
    return wire.VideoCodec.AV1;
  return wire.VideoCodec.VIDEO_CODEC_UNSPECIFIED;
}
export async function detectDesktopCodecs(): Promise<
  ultraterimal.wire.VideoCodec[]
> {
  const candidates = [
    { codec: "hvc1.1.6.L153.B0", family: wire.VideoCodec.HEVC },
    { codec: "av01.0.13M.08", family: wire.VideoCodec.AV1 },
  ];
  const results = await Promise.allSettled(
    candidates.map(async (candidate) => {
      const result = await VideoDecoder.isConfigSupported({
        codec: candidate.codec,
        codedWidth: 4096,
        codedHeight: 2160,
        optimizeForLatency: true,
        hardwareAcceleration: "prefer-hardware",
      });
      return result.supported ? candidate.family : undefined;
    }),
  );
  const codecs: ultraterimal.wire.VideoCodec[] = [wire.VideoCodec.AVC];
  for (const result of results)
    if (result.status === "fulfilled" && result.value !== undefined)
      codecs.push(result.value);
  return codecs;
}
