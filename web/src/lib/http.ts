export class ApiError extends Error {
  constructor(public readonly status: number) {
    super(
      status === 401
        ? "登录已过期，请重新登录"
        : status === 409
          ? "设备当前不可用或控制权已被占用"
          : "请求失败，请稍后重试",
    );
  }
}

export async function jsonRequest(
  path: string,
  body?: unknown,
): Promise<unknown> {
  const response = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    credentials: "same-origin",
    headers: body === undefined ? {} : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) throw new ApiError(response.status);
  return response.json();
}

export function record(value: unknown): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value))
    throw new Error("服务器响应格式错误");
  return value as Record<string, unknown>;
}

export function text(value: unknown): string {
  if (typeof value !== "string" || value.length > 4096)
    throw new Error("服务器响应字段错误");
  return value;
}
