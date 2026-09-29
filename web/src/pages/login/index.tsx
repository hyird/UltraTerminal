import { useState } from "react";
import { Button } from "@heroui/react";
import { useNavigate } from "react-router";
import { Field, Notice, failureText } from "../../components/form";
import { Icon } from "../../components/icon";
import { ApiError } from "../../lib/http";
import { login } from "../identity/identity.service";
import { useAuth } from "../../store/auth";
export function LoginPage() {
  const auth = useAuth();
  const navigate = useNavigate();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");
  const [step, setStep] = useState<"password" | "totp" | "recovery">(
    "password",
  );
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      const user = await login(
        username,
        password,
        step === "password" ? undefined : code,
      );
      setPassword("");
      setCode("");
      auth.accept(user);
      navigate("/overview", { replace: true });
    } catch (reason) {
      if (reason instanceof ApiError && reason.code === "factor_required") {
        setStep("totp");
        setCode("");
      } else
        setError(
          reason instanceof ApiError && reason.status === 401
            ? "账号、密码或验证码不正确，请重新输入"
            : failureText(reason),
        );
    } finally {
      setBusy(false);
    }
  }
  return (
    <main className="bg-surface-secondary flex min-h-dvh flex-col items-center justify-center px-6 py-12">
      <div className="mb-5 flex items-center gap-3 text-lg font-[650]">
        <span className="bg-accent text-accent-foreground flex size-8 items-center justify-center rounded-lg">
          <Icon name="terminal" />
        </span>
        <span>Ultraterimal</span>
      </div>
      <section className="bg-surface w-full max-w-[26rem] rounded-3xl border border-border p-6 max-md:p-7">
        <div className="mb-7">
          <p className="text-muted mb-2 text-sm">远程访问平台</p>
          <h1 className="text-2xl font-semibold tracking-tight">
            {step === "password"
              ? "登录工作台"
              : step === "totp"
                ? "验证您的身份"
                : "使用恢复码"}
          </h1>
          <p className="text-muted mt-3 text-sm leading-6">
            {step === "password"
              ? "使用平台账号，访问分配给您的设备。"
              : step === "totp"
                ? "输入 Google 验证器中的六位动态验证码。"
                : "输入绑定验证器时保存的一次性恢复码。"}
          </p>
        </div>
        <form className="flex flex-col gap-3" onSubmit={submit}>
          <Notice>{error}</Notice>
          {step === "password" ? (
            <>
              <Field
                label="用户名"
                value={username}
                onChange={setUsername}
                required
                maxLength={64}
                autoComplete="username"
              />
              <Field
                label="密码"
                type="password"
                value={password}
                onChange={setPassword}
                required
                autoComplete="current-password"
              />
            </>
          ) : (
            <Field
              key={step}
              label={step === "totp" ? "动态验证码" : "恢复码"}
              value={code}
              onChange={setCode}
              required
              maxLength={step === "totp" ? 6 : 32}
              pattern={step === "totp" ? "[0-9]{6}" : "[0-9a-fA-F]{32}"}
              autoComplete="one-time-code"
            />
          )}
          <Button
            size="sm"
            type="submit"
            className="mt-2 w-full"
            isPending={busy}
          >
            {step === "password" ? "登录" : "验证并登录"}
          </Button>
          {step !== "password" && (
            <div className="flex flex-wrap justify-between gap-1">
              <Button
                variant="ghost"
                size="sm"
                onPress={() => {
                  setCode("");
                  setError("");
                  setStep(step === "totp" ? "recovery" : "totp");
                }}
              >
                {step === "totp" ? "无法使用验证器" : "使用动态验证码"}
              </Button>
              <Button
                variant="ghost"
                size="sm"
                onPress={() => {
                  setStep("password");
                  setPassword("");
                  setCode("");
                  setError("");
                }}
              >
                返回登录
              </Button>
            </div>
          )}
        </form>
      </section>
      <p className="text-muted mt-7 text-center text-xs">
        账号由管理员分配 · 访问权限由平台校验
      </p>
    </main>
  );
}
