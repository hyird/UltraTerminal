import { Button, toast } from "@heroui/react";
import { useCallback, useEffect, useRef, useState } from "react";
import { Dialog } from "../../components/form";
import {
  deviceFileDownloadUrl,
  joinWindowsPath,
  listDeviceFiles,
  statDeviceFile,
  uploadDeviceFile,
  type ActiveUploadHandle,
  type DeviceFileEntry,
} from "./files.api";

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let value = bytes / 1024;
  let index = 0;
  while (value >= 1024 && index < units.length - 1) {
    value /= 1024;
    index++;
  }
  return `${value.toFixed(value >= 100 ? 0 : 1)} ${units[index]}`;
}

function formatModified(unixSeconds: number): string {
  if (!unixSeconds) return "-";
  return new Date(unixSeconds * 1000).toLocaleString();
}

interface UploadProgressState {
  name: string;
  loaded: number;
  total: number;
}

interface OverwritePromptState {
  file: File;
  targetPath: string;
  resolve: (overwrite: boolean) => void;
}

export function DesktopFilesPanel({
  deviceId,
  writeControl,
}: {
  deviceId: string;
  writeControl: boolean;
}) {
  const [currentPath, setCurrentPath] = useState("");
  const [parentPath, setParentPath] = useState("");
  const [entries, setEntries] = useState<DeviceFileEntry[]>([]);
  const [truncated, setTruncated] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [uploading, setUploading] = useState<UploadProgressState | null>(null);
  const [overwritePrompt, setOverwritePrompt] =
    useState<OverwritePromptState | null>(null);
  const [dragging, setDragging] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const activeUpload = useRef<ActiveUploadHandle | null>(null);
  const mounted = useRef(true);

  const loadDirectory = useCallback(
    async (targetPath: string) => {
      setLoading(true);
      setError("");
      try {
        const result = await listDeviceFiles(deviceId, targetPath);
        if (!mounted.current) return;
        setCurrentPath(result.normalizedPath);
        setParentPath(result.parentPath);
        setEntries(result.entries);
        setTruncated(result.truncated);
      } catch (err) {
        if (!mounted.current) return;
        setError(err instanceof Error ? err.message : "读取目录失败");
      } finally {
        if (mounted.current) setLoading(false);
      }
    },
    [deviceId],
  );

  useEffect(() => {
    mounted.current = true;
    void loadDirectory("");
    return () => {
      mounted.current = false;
      activeUpload.current?.abort();
    };
  }, [loadDirectory]);

  function askOverwrite(file: File, targetPath: string): Promise<boolean> {
    return new Promise<boolean>((resolve) => {
      setOverwritePrompt({ file, targetPath, resolve });
    });
  }

  async function handleUploadFiles(files: FileList | File[]) {
    if (!writeControl) {
      toast.danger("当前为只读会话，无权上传文件");
      return;
    }
    if (!currentPath) {
      toast.danger("请先进入目标磁盘或文件夹后再上传");
      return;
    }
    const list = Array.from(files);
    if (!list.length) return;
    let uploadedAny = false;
    for (const file of list) {
      if (!mounted.current) break;
      const targetPath = joinWindowsPath(currentPath, file.name);
      let overwrite = false;
      try {
        const info = await statDeviceFile(deviceId, targetPath);
        if (info.exists) {
          if (info.directory) {
            toast.danger(`目标路径“${file.name}”是同名目录，无法覆盖为文件`);
            continue;
          }
          const confirmed = await askOverwrite(file, targetPath);
          if (!confirmed) {
            continue;
          }
          overwrite = true;
        }
      } catch (err) {
        toast.danger(err instanceof Error ? err.message : "检查目标文件失败");
        continue;
      }

      setUploading({ name: file.name, loaded: 0, total: file.size });
      const handle = uploadDeviceFile(
        deviceId,
        targetPath,
        file,
        overwrite,
        (loaded, total) => {
          if (mounted.current) {
            setUploading({ name: file.name, loaded, total });
          }
        },
      );
      activeUpload.current = handle;
      try {
        await handle.promise;
        uploadedAny = true;
        toast.success(`已上传 ${file.name}`);
      } catch (err) {
        toast.danger(
          err instanceof Error ? err.message : `上传 ${file.name} 失败`,
        );
      } finally {
        activeUpload.current = null;
        if (mounted.current) setUploading(null);
      }
    }
    if (uploadedAny && mounted.current) {
      void loadDirectory(currentPath);
    }
  }

  function triggerDownload(entry: DeviceFileEntry) {
    const targetPath = joinWindowsPath(currentPath, entry.name);
    const link = document.createElement("a");
    link.href = deviceFileDownloadUrl(deviceId, targetPath);
    link.download = entry.directory ? `${entry.name}.zip` : entry.name;
    link.rel = "noopener";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }

  return (
    <>
      <div
        className={`flex flex-col gap-2.5 ${dragging ? "ring-2 ring-accent rounded-md" : ""}`}
        onDragOver={(event) => {
          if (!writeControl || !currentPath) return;
          event.preventDefault();
          setDragging(true);
        }}
        onDragLeave={() => setDragging(false)}
        onDrop={(event) => {
          event.preventDefault();
          setDragging(false);
          if (event.dataTransfer.files.length && !uploading) {
            void handleUploadFiles(event.dataTransfer.files);
          }
        }}
      >
        <div className="flex flex-wrap items-center gap-2">
          <Button
            size="sm"
            variant="tertiary"
            isDisabled={loading || !currentPath}
            onPress={() => void loadDirectory("")}
          >
            全部磁盘
          </Button>
          <Button
            size="sm"
            variant="tertiary"
            isDisabled={loading || !currentPath}
            onPress={() => void loadDirectory(parentPath)}
          >
            上一级
          </Button>
          <Button
            size="sm"
            variant="tertiary"
            isDisabled={loading}
            onPress={() => void loadDirectory(currentPath)}
          >
            刷新
          </Button>
          <div className="text-muted min-w-0 flex-1 truncate rounded bg-default/50 px-2.5 py-1 text-xs">
            {currentPath || "此电脑（磁盘列表）"}
          </div>
          {writeControl && (
            <>
              <input
                ref={fileInput}
                type="file"
                multiple
                hidden
                onChange={(event) => {
                  const files = event.target.files;
                  if (files && files.length) {
                    void handleUploadFiles(files);
                  }
                  event.target.value = "";
                }}
              />
              <Button
                size="sm"
                variant="primary"
                isDisabled={loading || !currentPath || Boolean(uploading)}
                onPress={() => fileInput.current?.click()}
              >
                上传文件
              </Button>
            </>
          )}
        </div>

        {uploading && (
          <div className="bg-default/40 flex items-center gap-3 rounded px-3 py-2 text-xs">
            <div className="min-w-0 flex-1">
              <div className="truncate font-medium">
                正在上传：{uploading.name}（
                {uploading.total > 0
                  ? `${Math.min(100, Math.round((uploading.loaded * 100) / uploading.total))}%`
                  : formatBytes(uploading.loaded)}
                ）
              </div>
              <div className="bg-default mt-1 h-1.5 w-full overflow-hidden rounded">
                <div
                  className="bg-accent h-full transition-all"
                  style={{
                    width: `${
                      uploading.total > 0
                        ? Math.min(
                            100,
                            Math.round(
                              (uploading.loaded * 100) / uploading.total,
                            ),
                          )
                        : 100
                    }%`,
                  }}
                />
              </div>
            </div>
            <Button
              size="sm"
              variant="danger-soft"
              onPress={() => activeUpload.current?.abort()}
            >
              取消
            </Button>
          </div>
        )}

        {error && (
          <div className="text-danger bg-danger/10 rounded px-3 py-2 text-xs">
            {error}
          </div>
        )}

        {truncated && (
          <div className="text-warning bg-warning/10 rounded px-3 py-1.5 text-xs">
            当前目录项过多，仅显示前 {entries.length} 项。
          </div>
        )}

        <div className="border-border max-h-[50vh] overflow-y-auto rounded border">
          <table className="w-full text-left text-xs">
            <thead className="bg-default/50 text-muted sticky top-0">
              <tr>
                <th className="px-3 py-2 font-medium">名称</th>
                <th className="w-24 px-3 py-2 font-medium">大小</th>
                <th className="w-44 px-3 py-2 font-medium max-sm:hidden">
                  修改时间
                </th>
                <th className="w-32 px-3 py-2 text-right font-medium">操作</th>
              </tr>
            </thead>
            <tbody className="divide-border divide-y">
              {loading ? (
                <tr>
                  <td colSpan={4} className="text-muted px-3 py-8 text-center">
                    正在加载目录…
                  </td>
                </tr>
              ) : entries.length === 0 ? (
                <tr>
                  <td colSpan={4} className="text-muted px-3 py-8 text-center">
                    目录为空
                  </td>
                </tr>
              ) : (
                entries.map((entry) => (
                  <tr key={entry.name} className="hover:bg-default/30">
                    <td className="max-w-0 truncate px-3 py-2">
                      {entry.directory ? (
                        <button
                          type="button"
                          className="text-accent hover:underline cursor-pointer text-left font-medium"
                          onClick={() =>
                            void loadDirectory(
                              joinWindowsPath(currentPath, entry.name),
                            )
                          }
                        >
                          {entry.name}
                        </button>
                      ) : (
                        <span>{entry.name}</span>
                      )}
                    </td>
                    <td className="text-muted px-3 py-2 whitespace-nowrap">
                      {entry.directory ? "文件夹" : formatBytes(entry.size)}
                    </td>
                    <td className="text-muted px-3 py-2 whitespace-nowrap max-sm:hidden">
                      {formatModified(entry.modifiedUnixSeconds)}
                    </td>
                    <td className="px-3 py-2 text-right whitespace-nowrap">
                      <Button
                        size="sm"
                        variant="tertiary"
                        onPress={() => triggerDownload(entry)}
                      >
                        {entry.directory ? "打包下载 .zip" : "下载"}
                      </Button>
                    </td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
        </div>
        {writeControl && currentPath && (
          <p className="text-muted text-xs">
            支持点击“上传文件”或直接拖拽本地文件到此面板上传；目录下载会自动流式打包为
            ZIP。
          </p>
        )}
      </div>

      {overwritePrompt && (
        <Dialog
          title="文件已存在"
          onClose={() => {
            overwritePrompt.resolve(false);
            setOverwritePrompt(null);
          }}
        >
          <div className="flex flex-col gap-4 text-sm">
            <p>
              目标目录中已存在同名文件{" "}
              <span className="font-semibold">{overwritePrompt.file.name}</span>
              ，是否覆盖？
            </p>
            <div className="flex justify-end gap-2">
              <Button
                variant="tertiary"
                onPress={() => {
                  overwritePrompt.resolve(false);
                  setOverwritePrompt(null);
                }}
              >
                跳过
              </Button>
              <Button
                variant="danger"
                onPress={() => {
                  overwritePrompt.resolve(true);
                  setOverwritePrompt(null);
                }}
              >
                覆盖
              </Button>
            </div>
          </div>
        </Dialog>
      )}
    </>
  );
}
