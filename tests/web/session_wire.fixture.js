export const ultraterimal = {
  wire: {
    Envelope: {
      encode(message) {
        return { finish: () => new TextEncoder().encode(JSON.stringify(message)) };
      },
      decode(bytes) {
        return JSON.parse(new TextDecoder().decode(bytes));
      },
    },
    DesktopProbe: { create(message) { return { acceptedResponseNonce: 0, ...message }; } },
    VideoConfig: { create(message) { return message; } },
    DisplayMode: { create(message) { return message; } },
    VideoFrame: { create(message) { return message; } },
    InputEvent: { Kind: { POINTER_MOVE: 1, BUTTON: 2, KEY: 3, WHEEL: 4, RELEASE_ALL: 5 } },
    QualityFeedback: { Preference: { BALANCED: 1 } },
  },
};
