import { LoadingSkeleton } from "../../components/loading";
import { useEffect, useState } from "react";
import { Button, toast } from "@heroui/react";
import {
  Choice,
  Dialog,
  Field,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import {
  createDevice,
  createEnrollment,
  deleteDevice,
  renameDevice,
  transferDeviceOwner,
  updateAgent,
  windowsLogin,
} from "./device.service";
import { onboardingCommand } from "./device.install";
import { listAccounts } from "../identity/identity.service";
import { ProofFields } from "../identity/proof";
import type { Account } from "../identity/identity.types";
import type { Device } from "./device.types";
export type DeviceAction =
  | "create"
  | "rename"
  | "transfer"
  | "remove"
  | "install"
  | "credentials"
  | "update";
export function DeviceActionDialog({
  action,
  device,
  onClose,
  onCreated,
}: {
  action: DeviceAction;
  device?: Device;
  onClose: () => void;
  onCreated?: (device: Device) => void;
}) {
  const [name, setName] = useState(
    action === "rename" ? (device?.name ?? "") : "",
  );
  const [username, setUsername] = useState("");
  const [domain, setDomain] = useState("");
  const [password, setPassword] = useState("");
  const [accounts, setAccounts] = useState<Account[]>([]);
  const [ownerId, setOwnerId] = useState("");
  const [currentPassword, setCurrentPassword] = useState("");
  const [code, setCode] = useState("");
  const [telegramProof, setTelegramProof] = useState("");
  const [saved, setSaved] = useState(false);
  const [command, setCommand] = useState("");
  const [loading, setLoading] = useState(
    action === "credentials" || action === "transfer",
  );
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    if (action !== "credentials" || !device) return;
    let live = true;
    windowsLogin(device.id, "read")
      .then((result) => {
        if (live) {
          setUsername(result.username);
          setDomain(result.domain);
          setSaved(result.saved);
        }
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, [action, device]);
  useEffect(() => {
    if (action !== "transfer") return;
    let live = true;
    listAccounts()
      .then((items) => {
        if (live)
          setAccounts(
            items.filter(
              (item) =>
                !item.disabled && item.username !== device?.ownerUsername,
            ),
          );
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, [action, device]);
  const titles: Record<DeviceAction, string> = {
    create: "添加 Windows 设备",
    rename: "重命名设备",
    transfer: "修改所有者",
    remove: "删除设备",
    install: "安装与接入",
    credentials: "Windows 登录凭据",
    update: "更新 Agent",
  };
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy || loading) return;
    if (action === "transfer" && !ownerId) {
      setError("请选择新所有者");
      return;
    }
    setBusy(true);
    setError("");
    try {
      if (action === "create") {
        const created = await createDevice(name.trim());
        onCreated?.(created);
        toast.success("设备已创建，请安装 Agent 完成接入");
        return;
      }
      if (!device) return;
      if (action === "install") {
        setCommand(
          onboardingCommand(await createEnrollment(device.id, "windows-x64")),
        );
        return;
      }
      if (action === "rename") await renameDevice(device.id, name.trim());
      else if (action === "transfer") {
        await transferDeviceOwner(device.id, ownerId, {
          password: currentPassword,
          code,
          telegram_proof: telegramProof,
        });
        toast.success("设备所有者已修改");
        onClose();
        return;
      } else if (action === "remove") await deleteDevice(device.id);
      else if (action === "credentials")
        await windowsLogin(device.id, "save", {
          username,
          password,
          domain,
        });
      else if (action === "update") {
        const result = await updateAgent(device.id);
        toast.success(
          result.status === "current"
            ? `已是最新版本 ${result.version}`
            : `已请求升级到 ${result.version}`,
        );
        onClose();
        return;
      }
      toast.success("设备信息已更新");
      onClose();
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  return (
    <Dialog
      title={device ? `${titles[action]} · ${device.name}` : titles[action]}
      onClose={onClose}
      busy={busy}
    >
      <form className="flex flex-col gap-3" onSubmit={submit}>
        <Notice>{error}</Notice>
        {(action === "create" || action === "rename") && (
          <Field
            label="设备名称"
            value={name}
            onChange={setName}
            required
            maxLength={128}
            hint={
              action === "create"
                ? "使用容易识别的名称，例如办公室工作站。"
                : undefined
            }
          />
        )}
        {action === "create" && (
          <p className="text-muted text-sm leading-6">
            先创建设备，再在目标 Windows
            电脑执行一次性安装命令。接入成功后可向其他用户分配访问权限。
          </p>
        )}
        {action === "transfer" &&
          (loading ? (
            <LoadingSkeleton kind="form" label="正在加载账号" rows={3} />
          ) : (
            <>
              <p className="text-muted text-sm leading-6">
                当前所有者：{device?.ownerUsername ?? "—"}
                。修改后，原所有者的设备授权和未使用安装码将失效；新所有者获得连接与设备管理权限。
              </p>
              <Choice
                label="新所有者"
                value={ownerId}
                onChange={setOwnerId}
                items={accounts.map((item) => ({
                  id: item.id,
                  label: item.username,
                }))}
              />
              {accounts.length === 0 && (
                <p className="text-muted text-sm">没有可用的其他账号</p>
              )}
              <ProofFields
                password={currentPassword}
                code={code}
                onPassword={setCurrentPassword}
                onCode={setCode}
                telegramProof={telegramProof}
                onTelegramProof={setTelegramProof}
              />
            </>
          ))}
        {action === "remove" && (
          <p className="text-sm leading-7">
            删除后会撤销此设备凭据及用户授权，并关闭正在运行的远控会话。历史审计记录保留。重新接入需要重新注册。
          </p>
        )}
        {action === "update" && (
          <p className="text-sm leading-7">
            将安装平台发布的最新
            Agent，期间设备会短暂断开并自动重连，现有身份和凭据会保留。
          </p>
        )}
        {action === "install" && (
          <>
            <p className="text-muted text-sm leading-6">
              在目标 Windows 电脑的管理员 PowerShell
              中执行安装命令。新设备的一次性命令十分钟内有效，重新生成会使旧命令失效。
            </p>
            {command && (
              <>
                <textarea
                  aria-label="安装命令"
                  className="bg-surface-secondary w-full resize-y rounded-lg border border-border p-4 font-mono text-xs leading-7"
                  readOnly
                  value={command}
                  rows={5}
                />
                <Button
                  size="sm"
                  variant="outline"
                  onPress={() =>
                    void navigator.clipboard
                      .writeText(command)
                      .then(() => toast.success("安装命令已复制"))
                      .catch(() => setError("复制失败，请手动选择命令"))
                  }
                >
                  复制命令
                </Button>
              </>
            )}
          </>
        )}
        {action === "credentials" && (
          <>
            {loading ? (
              <LoadingSkeleton kind="form" label="正在读取凭据状态" rows={3} />
            ) : (
              <>
                <p className="text-muted text-sm leading-6">
                  凭据加密保存在目标 Windows 设备，用于控制台登录与解锁。
                  {saved
                    ? "密码留空可保留原值。"
                    : "首次保存需要用户名和密码。"}
                </p>
                <Field
                  label="Windows 用户名"
                  value={username}
                  onChange={setUsername}
                  required={!saved}
                  autoComplete="off"
                />
                <Field
                  label="域（可选）"
                  value={domain}
                  onChange={setDomain}
                  maxLength={256}
                  autoComplete="off"
                />
                <Field
                  label="Windows 密码"
                  type="password"
                  value={password}
                  onChange={setPassword}
                  required={!saved}
                  autoComplete="new-password"
                />
              </>
            )}
          </>
        )}
        <FormActions
          onCancel={onClose}
          busy={busy || loading}
          danger={action === "remove"}
          label={
            action === "create"
              ? "创建设备"
              : action === "remove"
                ? "删除设备"
                : action === "install"
                  ? command
                    ? "重新生成"
                    : "生成安装命令"
                  : action === "update"
                    ? "确认更新"
                    : action === "transfer"
                      ? "确认修改"
                      : "保存"
          }
        />
      </form>
    </Dialog>
  );
}
