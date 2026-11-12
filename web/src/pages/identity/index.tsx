import { LoadingSkeleton } from "../../components/loading";
import { useEffect, useState } from "react";
import { Button, Chip, toast } from "@heroui/react";
import QRCode from "qrcode";
import { Page } from "../../layout/page";
import {
  Dialog,
  Field,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import { Icon, type IconName } from "../../components/icon";
import { useAuth } from "../../store/auth";
import {
  bindTelegram,
  changeSecurity,
  loadTelegram,
  observeAccount,
  securityInfo,
  unbindTelegram,
} from "./identity.service";
import type { SecurityInfo, TelegramInfo } from "./identity.types";
import { ProofFields } from "./proof";
type Action =
  "begin" | "confirm" | "disable" | "recovery" | "password" | "bind" | "unbind";
const titles: Record<Action, string> = {
  begin: "绑定 Google 验证器",
  confirm: "确认动态验证码",
  disable: "关闭双重验证",
  recovery: "重新生成恢复码",
  password: "修改密码",
  bind: "绑定 Telegram",
  unbind: "解绑 Telegram",
};
export function SecurityPage() {
  const { user } = useAuth();
  const [security, setSecurity] = useState<SecurityInfo | null>(null);
  const [telegram, setTelegram] = useState<TelegramInfo | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [action, setAction] = useState<Action | null>(null);
  const [recovery, setRecovery] = useState<string[]>([]);
  const [binding, setBinding] = useState("");
  useEffect(() => {
    let live = true;
    Promise.all([securityInfo(), loadTelegram()])
      .then(([s, t]) => {
        if (live) {
          setSecurity(s);
          setTelegram(t);
        }
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    const stop = observeAccount((state) => {
      if (live) {
        setTelegram(state);
        if (state.telegramId) setBinding("");
      }
    }, setError);
    return () => {
      live = false;
      stop();
    };
  }, []);
  async function refreshed() {
    try {
      const [s, t] = await Promise.all([securityInfo(), loadTelegram()]);
      setSecurity(s);
      setTelegram(t);
    } catch (error) {
      setError(failureText(error));
    }
  }
  return (
    <Page
      title="个人安全"
      description="保护您的平台账号，并绑定用于设备连接验证的 Telegram。"
    >
      <Notice>{error}</Notice>
      {loading ? (
        <LoadingSkeleton kind="settings" label="正在加载安全设置" />
      ) : (
        <div className="mx-auto flex w-full max-w-5xl flex-col gap-3">
          <div className="flex items-center gap-3 border-b border-border py-3 wrap-anywhere">
            <span className="bg-accent-soft text-accent-soft-foreground inline-flex size-12 shrink-0 items-center justify-center rounded-full text-xl">
              {user?.username.slice(0, 1).toUpperCase()}
            </span>
            <div>
              <h2 className="text-lg font-semibold">{user?.username}</h2>
              <p className="text-muted mt-1 text-sm">
                {user?.role === "admin" ? "平台管理员" : "普通用户"}
              </p>
            </div>
          </div>
          <SecurityRow
            icon="key"
            title="登录密码"
            description="定期更换密码，避免在不同平台重复使用。"
          >
            <Button
              size="sm"
              variant="outline"
              onPress={() => setAction("password")}
            >
              修改密码
            </Button>
          </SecurityRow>
          <SecurityRow
            icon="shield-check"
            title="双重验证"
            description="使用 Google 验证器，在密码之外增加一道验证。"
          >
            <Chip
              size="sm"
              variant="soft"
              color={security?.totp ? "success" : "default"}
            >
              {security ? (security.totp ? "已开启" : "未开启") : "加载中"}
            </Chip>
            <Button
              size="sm"
              variant="outline"
              isDisabled={!security}
              onPress={() => setAction(security?.totp ? "disable" : "begin")}
            >
              {security?.totp ? "关闭验证" : "绑定验证器"}
            </Button>
          </SecurityRow>
          {security?.totp && (
            <SecurityRow
              icon="file-text"
              title="恢复码"
              description={`剩余 ${security.recoveryCodes} 个一次性恢复码。重新生成会使旧码全部失效。`}
            >
              <Button
                size="sm"
                variant="outline"
                onPress={() => setAction("recovery")}
              >
                重新生成
              </Button>
            </SecurityRow>
          )}
          <SecurityRow
            icon="paper-plane"
            title="Telegram"
            description={
              telegram?.telegramId
                ? `已绑定账号 ${telegram.telegramId}，设备验证将通过 Bot 通知。`
                : telegram?.botUsername
                  ? `连接 @${telegram.botUsername}，接收并处理设备验证。`
                  : "管理员配置平台 Bot 后，即可绑定 Telegram。"
            }
          >
            <Chip
              size="sm"
              variant="soft"
              color={telegram?.telegramId ? "success" : "default"}
            >
              {telegram?.telegramId ? "已绑定" : "未绑定"}
            </Chip>
            <Button
              size="sm"
              variant="outline"
              isDisabled={!telegram?.botUsername}
              onPress={() =>
                setAction(telegram?.telegramId ? "unbind" : "bind")
              }
            >
              {telegram?.telegramId ? "解绑" : "绑定 Telegram"}
            </Button>
          </SecurityRow>
        </div>
      )}
      {binding && (
        <div className="flex flex-col gap-3 p-6">
          <h2 className="font-medium">完成 Telegram 绑定</h2>
          <p className="text-muted my-3 text-sm">
            在 Telegram 私聊中点击 Start 完成绑定，链接十分钟内有效。
          </p>
          <a
            className="text-accent font-medium"
            href={binding}
            target="_blank"
            rel="noopener noreferrer"
          >
            前往 Telegram →
          </a>
        </div>
      )}
      {action && (
        <SecurityAction
          initial={action}
          username={user?.username ?? ""}
          totp={security?.totp ?? false}
          onClose={() => setAction(null)}
          onSaved={(codes, url) => {
            setAction(null);
            setRecovery(codes);
            if (url) setBinding(url);
            void refreshed();
          }}
        />
      )}
      {recovery.length > 0 && (
        <Dialog title="保存一次性恢复码" onClose={() => setRecovery([])}>
          <p className="text-muted mb-4 text-sm">
            恢复码只显示这一次，请保存在安全的位置。每个码只能使用一次。
          </p>
          <div className="bg-surface-secondary grid gap-2 rounded-lg p-4 text-[0.8125rem] wrap-anywhere">
            {recovery.map((code) => (
              <code key={code}>{code}</code>
            ))}
          </div>
          <div className="mt-5 flex flex-wrap justify-end gap-2">
            <Button
              size="sm"
              variant="outline"
              onPress={() =>
                void navigator.clipboard
                  .writeText(recovery.join("\n"))
                  .then(() => toast.success("已复制恢复码"))
                  .catch(() => toast.danger("无法复制，请手动保存"))
              }
            >
              复制恢复码
            </Button>
            <Button size="sm" onPress={() => setRecovery([])}>
              我已保存
            </Button>
          </div>
        </Dialog>
      )}
    </Page>
  );
}
function SecurityRow({
  icon,
  title,
  description,
  children,
}: {
  icon: IconName;
  title: string;
  description: string;
  children: React.ReactNode;
}) {
  return (
    <div className="grid grid-cols-1 items-center gap-3 border-b border-border py-3 last:border-b-0 @min-[38rem]:grid-cols-[minmax(0,1fr)_auto] @min-[38rem]:gap-6">
      <div className="flex min-w-0 items-start gap-4">
        <span className="text-muted mt-1 shrink-0">
          <Icon name={icon} />
        </span>
        <div className="min-w-0 wrap-anywhere">
          <h2 className="font-medium">{title}</h2>
          <p className="text-muted mt-2 max-w-xl text-sm leading-6">
            {description}
          </p>
        </div>
      </div>
      <div className="flex flex-wrap items-center justify-end gap-3">
        {children}
      </div>
    </div>
  );
}
function SecurityAction({
  initial,
  username,
  totp,
  onClose,
  onSaved,
}: {
  initial: Action;
  username: string;
  totp: boolean;
  onClose: () => void;
  onSaved: (codes: string[], url?: string) => void;
}) {
  const [action, setAction] = useState(initial);
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");
  const [telegramProof, setTelegramProof] = useState("");
  const [newPassword, setNewPassword] = useState("");
  const [confirmPassword, setConfirmPassword] = useState("");
  const [secret, setSecret] = useState("");
  const [qr, setQr] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    let live = true;
    if (secret)
      void QRCode.toDataURL(
        `otpauth://totp/Ultraterimal:${encodeURIComponent(username)}?secret=${encodeURIComponent(secret)}&issuer=Ultraterimal&algorithm=SHA1&digits=6&period=30`,
        { width: 220, margin: 1 },
      )
        .then((value) => {
          if (live) setQr(value);
        })
        .catch(() => {
          if (live) setError("无法生成二维码，请手动输入下方密钥");
        });
    return () => {
      live = false;
    };
  }, [secret, username]);
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    if (action === "password" && newPassword !== confirmPassword) {
      setError("两次输入的新密码不一致");
      return;
    }
    setBusy(true);
    setError("");
    try {
      const proof = { password, code, telegram_proof: telegramProof };
      if (action === "bind") {
        onSaved([], await bindTelegram(proof));
      } else if (action === "unbind") {
        await unbindTelegram(proof);
        onSaved([]);
      } else {
        const result = await changeSecurity(
          action,
          proof,
          newPassword || undefined,
        );
        if (action === "begin") {
          setSecret(result.secret);
          setAction("confirm");
          setPassword("");
          setTelegramProof("");
          setCode("");
          return;
        }
        onSaved(result.recoveryCodes);
      }
      toast.success("安全设置已更新");
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  return (
    <Dialog title={titles[action]} onClose={onClose} busy={busy}>
      <form onSubmit={submit} className="flex flex-col gap-3">
        <Notice>{error}</Notice>
        {secret && (
          <>
            <p className="text-muted text-sm">
              使用 Google 验证器扫描二维码，然后输入当前验证码完成绑定。
            </p>
            {qr && (
              <img
                src={qr}
                alt="Google 验证器绑定二维码"
                width={220}
                height={220}
                className="mx-auto"
              />
            )}
            <code className="break-all rounded-lg bg-surface-secondary p-3 text-center text-sm">
              {secret}
            </code>
          </>
        )}
        {action === "password" && (
          <>
            <Field
              label="新密码"
              type="password"
              value={newPassword}
              onChange={setNewPassword}
              required
              minLength={12}
              autoComplete="new-password"
            />
            <Field
              label="确认新密码"
              type="password"
              value={confirmPassword}
              onChange={setConfirmPassword}
              required
              minLength={12}
              autoComplete="new-password"
            />
          </>
        )}
        {action === "disable" && (
          <p className="text-warning text-sm">关闭后，账号将仅使用密码登录。</p>
        )}
        {action === "unbind" && (
          <p className="text-warning text-sm">解绑会使待处理的连接验证失效。</p>
        )}
        <ProofFields
          password={password}
          code={code}
          onPassword={setPassword}
          onCode={setCode}
          telegramProof={telegramProof}
          onTelegramProof={setTelegramProof}
          requireCode={totp}
          confirmBinding={action === "confirm"}
        />
        <FormActions
          onCancel={onClose}
          busy={busy}
          label={action === "begin" ? "下一步" : "确认"}
          danger={action === "disable" || action === "unbind"}
        />
      </form>
    </Dialog>
  );
}
