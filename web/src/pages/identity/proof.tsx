import { Button } from "@heroui/react";
import { identityApi } from "./identity.api";
import { record, text } from "../../lib/http";
import { loadTelegram } from "./identity.service";
import { useEffect, useState } from "react";
import { LoadingSkeleton } from "../../components/loading";
import { Choice, Field, Notice, failureText } from "../../components/form";
import { securityInfo } from "./identity.service";
export function ProofFields({
  password,
  code,
  onPassword,
  onCode,
  requireCode,
  confirmBinding = false,
  telegramProof,
  onTelegramProof,
}: {
  password: string;
  code: string;
  onPassword: (value: string) => void;
  onCode: (value: string) => void;
  requireCode?: boolean;
  confirmBinding?: boolean;
  telegramProof: string;
  onTelegramProof: (value: string) => void;
}) {
  const [totp, setTotp] = useState<boolean | null>(requireCode ?? null);
  const [telegram, setTelegram] = useState(false);
  const [sending, setSending] = useState(false);
  useEffect(() => {
    let live = true;
    loadTelegram()
      .then((info) => {
        if (live) setTelegram(!!info.botUsername && !!info.telegramId);
      })
      .catch(() => {});
    return () => {
      live = false;
    };
  }, []);
  async function requestTelegram() {
    setSending(true);
    setError("");
    try {
      onTelegramProof(text(record(await identityApi.verifyTelegram()).id));
    } catch (error) {
      setError(failureText(error));
    } finally {
      setSending(false);
    }
  }
  const [method, setMethod] = useState("password");
  const [error, setError] = useState("");
  useEffect(() => {
    if (requireCode !== undefined) {
      setTotp(requireCode);
      return;
    }
    let live = true;
    securityInfo()
      .then((info) => {
        if (live) setTotp(info.totp);
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      });
    return () => {
      live = false;
    };
  }, [requireCode]);
  if (totp === null && !error && !confirmBinding)
    return <LoadingSkeleton kind="form" label="正在加载身份验证" rows={2} />;
  const useCode = confirmBinding || (totp === true && method === "totp");
  return (
    <div>
      <p className="text-muted mb-2 text-xs">
        {confirmBinding
          ? "输入新绑定验证器的动态码"
          : "验证当前账号以确认此项操作"}
      </p>
      <div className="flex flex-col gap-3">
        <Notice>{error}</Notice>
        {!confirmBinding && (
          <Choice
            label="验证方式"
            value={method}
            onChange={(value) => {
              setMethod(value);
              onPassword("");
              onCode("");
              onTelegramProof("");
            }}
            items={[
              { id: "password", label: "账号密码" },
              ...(totp ? [{ id: "totp", label: "Google 验证器" }] : []),
              { id: "telegram", label: "Telegram" },
            ]}
          />
        )}
        {method === "telegram" && !confirmBinding ? (
          <div className="space-y-3">
            <p className="text-muted text-sm">
              {telegram
                ? telegramProof
                  ? "已发送确认请求，请在 Telegram 中确认，然后点击下方确认继续。请求在 5 分钟后失效。"
                  : "向当前账号绑定的 Telegram 发送身份确认请求。"
                : "请先在个人安全页面绑定 Telegram；平台管理员需先配置 Bot。"}
            </p>
            <Button
              type="button"
              variant="secondary"
              isDisabled={!telegram || sending || !!telegramProof}
              onPress={() => void requestTelegram()}
            >
              {sending
                ? "正在发送"
                : telegramProof
                  ? "等待 Telegram 确认"
                  : "发送确认请求"}
            </Button>
          </div>
        ) : useCode ? (
          <Field
            label="动态验证码"
            required
            value={code}
            onChange={onCode}
            minLength={6}
            maxLength={6}
            pattern="[0-9]{6}"
            autoComplete="one-time-code"
            inputMode="numeric"
            name="verification_code"
          />
        ) : (
          <Field
            label="当前账号密码"
            type="password"
            required
            value={password}
            onChange={onPassword}
            autoComplete="current-password"
          />
        )}
      </div>
    </div>
  );
}
