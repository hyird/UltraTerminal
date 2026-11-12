import { LoadingSkeleton } from "../../components/loading";
import { useEffect, useState } from "react";
import { Button, Chip, toast } from "@heroui/react";
import { Page } from "../../layout/page";
import {
  Dialog,
  Field,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import { configureTelegram, loadTelegram } from "./identity.service";
import { ProofFields } from "./proof";
export function SettingsPage() {
  const [bot, setBot] = useState("");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [action, setAction] = useState<"configure" | "disable" | null>(null);
  async function refresh() {
    try {
      setBot((await loadTelegram()).botUsername);
    } catch (error) {
      setError(failureText(error));
    } finally {
      setLoading(false);
    }
  }
  useEffect(() => {
    void refresh();
  }, []);
  return (
    <Page
      title="平台设置"
      description="配置设备连接验证所需的平台服务。个人 Telegram 账号在个人安全中绑定。"
    >
      <Notice>{error}</Notice>
      {loading ? (
        <LoadingSkeleton kind="settings" label="正在加载平台设置" rows={2} />
      ) : (
        <div className="flex flex-col gap-3">
          <div className="flex flex-wrap items-start justify-between gap-4">
            <div>
              <h2 className="text-lg font-semibold">Telegram 验证服务</h2>
              <p className="text-muted mt-2 max-w-2xl text-sm leading-6">
                按用户的设备授权配置直接连接或二级验证。Telegram
                验证发送给发起连接的用户本人，仅申请人需要绑定 Telegram。
              </p>
            </div>
            <Chip color={bot ? "success" : "default"} variant="soft" size="sm">
              {loading ? "加载中" : bot ? "已配置" : "未配置"}
            </Chip>
          </div>
          <dl className="my-3 grid gap-4 @min-[30rem]:grid-cols-2">
            <div>
              <dt className="text-muted text-xs">平台 Bot</dt>
              <dd className="mt-2 text-sm font-medium">
                {bot ? `@${bot}` : "尚未连接"}
              </dd>
            </div>
            <div>
              <dt className="text-muted text-xs">验证接收人</dt>
              <dd className="mt-2 text-sm font-medium">发起连接的当前账号</dd>
            </div>
          </dl>
          <div className="flex flex-wrap gap-3">
            <Button
              size="sm"
              onPress={() => setAction("configure")}
              isDisabled={loading}
            >
              {bot ? "更换 Bot" : "配置 Bot"}
            </Button>
            {bot && (
              <Button
                size="sm"
                variant="danger-soft"
                onPress={() => setAction("disable")}
              >
                停用服务
              </Button>
            )}
          </div>
          <p className="text-muted mt-5 text-xs">
            先通过 Telegram 的 @BotFather 创建 Bot，再在此录入 Token。Token
            保存后不会回显。
          </p>
        </div>
      )}
      {action && (
        <BotEditor
          disable={action === "disable"}
          onClose={() => setAction(null)}
          onSaved={() => {
            setAction(null);
            void refresh();
          }}
        />
      )}
    </Page>
  );
}
function BotEditor({
  disable,
  onClose,
  onSaved,
}: {
  disable: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [token, setToken] = useState("");
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");
  const [telegramProof, setTelegramProof] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await configureTelegram(token, disable, {
        password,
        code,
        telegram_proof: telegramProof,
      });
      toast.success(disable ? "验证服务已停用" : "Bot 已配置");
      onSaved();
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  return (
    <Dialog
      title={disable ? "停用验证服务" : "配置 Telegram Bot"}
      onClose={onClose}
      busy={busy}
    >
      <form className="flex flex-col gap-3" onSubmit={submit}>
        <Notice>{error}</Notice>
        {disable ? (
          <p className="text-warning text-sm">
            停用后，需要验证的设备连接将无法确认，未使用的验证会失效。
          </p>
        ) : (
          <Field
            label="Bot Token"
            type="password"
            value={token}
            onChange={setToken}
            required
            autoComplete="off"
          />
        )}
        <ProofFields
          password={password}
          code={code}
          onPassword={setPassword}
          onCode={setCode}
          telegramProof={telegramProof}
          onTelegramProof={setTelegramProof}
        />
        <FormActions
          onCancel={onClose}
          busy={busy}
          danger={disable}
          label={disable ? "确认停用" : "连接 Bot"}
        />
      </form>
    </Dialog>
  );
}
