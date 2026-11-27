import { expect, test } from "bun:test";
import {
  buildMcpClientConfig,
  parseApiTokenList,
} from "../../web/src/pages/identity/identity.service";

test("buildMcpClientConfig normalizes trailing slash and formats Bearer config", () => {
  const json = buildMcpClientConfig(
    "https://test.a-z.xin/",
    "utm_0123456789abcdef",
  );
  expect(JSON.parse(json)).toEqual({
    mcpServers: {
      ultraterimal: {
        url: "https://test.a-z.xin/api/mcp",
        headers: {
          Authorization: "Bearer utm_0123456789abcdef",
        },
      },
    },
  });
});

test("parseApiTokenList validates token payloads and rejects unknown scopes", () => {
  const parsed = parseApiTokenList([
    {
      id: "tok-1",
      name: "AI Bot",
      prefix: "utm_01234567",
      scopes: ["devices:read", "desktop:read", "desktop:input"],
      device_ids: ["d-1"],
      expires_at: null,
      last_used_at: "2026-11-28T00:00:00.000000Z",
      created_at: "2026-11-28T00:00:00.000000Z",
    },
  ]);
  expect(parsed).toEqual([
    {
      id: "tok-1",
      name: "AI Bot",
      prefix: "utm_01234567",
      scopes: ["devices:read", "desktop:read", "desktop:input"],
      deviceIds: ["d-1"],
      expiresAt: null,
      lastUsedAt: "2026-11-28T00:00:00.000000Z",
      createdAt: "2026-11-28T00:00:00.000000Z",
    },
  ]);

  expect(() =>
    parseApiTokenList([
      {
        id: "tok-2",
        name: "Bad",
        prefix: "utm_bad",
        scopes: ["root:all"],
        device_ids: [],
        expires_at: null,
        last_used_at: null,
        created_at: "2026-11-28T00:00:00.000000Z",
      },
    ]),
  ).toThrow();
});
