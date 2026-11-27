import { ApiError, jsonRequest, record, text, xsrfToken } from "../../lib/http";

export interface DeviceFileEntry {
  name: string;
  directory: boolean;
  size: number;
  modifiedUnixSeconds: number;
}

export interface DeviceFileList {
  normalizedPath: string;
  parentPath: string;
  truncated: boolean;
  entries: DeviceFileEntry[];
}

export interface DeviceFileStat {
  exists: boolean;
  directory: boolean;
  size: number;
  name: string;
}

export interface DeviceFileUploadResult {
  filename: string;
  bytesTransferred: number;
}

export interface ActiveUploadHandle {
  promise: Promise<DeviceFileUploadResult>;
  abort: () => void;
}

function numberField(value: unknown): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new Error("服务器响应字段错误");
  }
  return value;
}

function boolField(value: unknown): boolean {
  if (typeof value !== "boolean") {
    throw new Error("服务器响应字段错误");
  }
  return value;
}

export async function listDeviceFiles(
  deviceId: string,
  sessionId: string,
  path: string,
): Promise<DeviceFileList> {
  const query = new URLSearchParams({
    device_id: deviceId,
    session_id: sessionId,
    path,
  });
  const raw = record(
    await jsonRequest(`/api/devices/files/list?${query.toString()}`),
  );
  const rawEntries = Array.isArray(raw.entries) ? raw.entries : [];
  return {
    normalizedPath: text(raw.normalized_path),
    parentPath: text(raw.parent_path),
    truncated: boolField(raw.truncated),
    entries: rawEntries.map((item) => {
      const entry = record(item);
      return {
        name: text(entry.name),
        directory: boolField(entry.directory),
        size: numberField(entry.size),
        modifiedUnixSeconds: numberField(entry.modified_unix_seconds),
      };
    }),
  };
}

export async function statDeviceFile(
  deviceId: string,
  sessionId: string,
  path: string,
): Promise<DeviceFileStat> {
  const query = new URLSearchParams({
    device_id: deviceId,
    session_id: sessionId,
    path,
  });
  const raw = record(
    await jsonRequest(`/api/devices/files/stat?${query.toString()}`),
  );
  return {
    exists: boolField(raw.exists),
    directory: boolField(raw.directory),
    size: numberField(raw.size),
    name: text(raw.name),
  };
}

export function deviceFileDownloadUrl(
  deviceId: string,
  sessionId: string,
  path: string,
): string {
  const query = new URLSearchParams({
    device_id: deviceId,
    session_id: sessionId,
    path,
  });
  return `/api/devices/files/download?${query.toString()}`;
}

export function joinWindowsPath(base: string, name: string): string {
  if (!base) return name;
  return base.endsWith("\\") ? `${base}${name}` : `${base}\\${name}`;
}

export function uploadDeviceFile(
  deviceId: string,
  sessionId: string,
  targetPath: string,
  file: File,
  overwrite: boolean,
  onProgress?: (loaded: number, total: number) => void,
): ActiveUploadHandle {
  const xhr = new XMLHttpRequest();
  const query = new URLSearchParams({
    device_id: deviceId,
    session_id: sessionId,
    path: targetPath,
    overwrite: overwrite ? "1" : "0",
  });
  const promise = new Promise<DeviceFileUploadResult>((resolve, reject) => {
    xhr.open("POST", `/api/devices/files/upload?${query.toString()}`, true);
    xhr.withCredentials = true;
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    const token = xsrfToken();
    if (token) {
      xhr.setRequestHeader("X-XSRF-TOKEN", token);
    }
    xhr.upload.onprogress = (event) => {
      if (onProgress) {
        onProgress(
          event.loaded,
          event.lengthComputable ? event.total : file.size,
        );
      }
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try {
          const parsed = record(JSON.parse(xhr.responseText));
          resolve({
            filename: text(parsed.filename),
            bytesTransferred: numberField(parsed.bytes_transferred),
          });
        } catch (error) {
          reject(
            error instanceof Error ? error : new Error("服务器响应格式错误"),
          );
        }
        return;
      }
      let code = "";
      try {
        const parsed = record(JSON.parse(xhr.responseText));
        if (typeof parsed.code === "string") code = parsed.code;
      } catch {
        /* Keep status-based fallback */
      }
      reject(new ApiError(xhr.status, code));
    };
    xhr.onerror = () => reject(new Error("网络异常，文件上传失败"));
    xhr.onabort = () => reject(new Error("文件上传已取消"));
    xhr.send(file);
  });
  return {
    promise,
    abort: () => xhr.abort(),
  };
}
