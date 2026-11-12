import { useEffect, useState } from "react";
import { Button } from "@heroui/react";
import { useNavigate } from "react-router";
import { Field, Notice, failureText } from "../../components/form";
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
  useEffect(() => {
    document.title = `${step === "password" ? "登录" : "身份验证"} · Ultraterimal`;
  }, [step]);
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
    <main className="bg-surface-secondary flex min-h-dvh items-center justify-center px-6 py-12">
      <section className="bg-surface w-full max-w-[26rem] rounded-3xl border border-border p-6 max-md:p-7">
        <div className="mb-7">
          <h1 className="text-2xl font-semibold tracking-tight">
            {step === "password"
              ? "登录"
              : step === "totp"
                ? "验证您的身份"
                : "使用恢复码"}
          </h1>
          {step !== "password" && (
            <p className="text-muted mt-3 text-sm leading-6">
              {step === "totp"
                ? "输入 Google 验证器中的六位动态验证码。"
                : "输入绑定验证器时保存的一次性恢复码。"}
            </p>
          )}
        </div>
        <form
          aria-label="账号登录"
          className="flex flex-col gap-3"
          onSubmit={submit}
        >
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
                name="username"
              />
              <Field
                label="密码"
                type="password"
                value={password}
                onChange={setPassword}
                required
                autoComplete="current-password"
                name="password"
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
              name="verification_code"
              inputMode={step === "totp" ? "numeric" : "text"}
              autoFocus
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
    </main>
  );
}
