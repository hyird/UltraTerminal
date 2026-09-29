import { useEffect, useState } from "react";
import { Field, Notice, failureText } from "../../components/form";
import { securityInfo } from "./identity.service";
export function ProofFields({
  password,
  code,
  onPassword,
  onCode,
  requireCode,
}: {
  password: string;
  code: string;
  onPassword: (value: string) => void;
  onCode: (value: string) => void;
  requireCode?: boolean;
}) {
  const [totp, setTotp] = useState<boolean | null>(requireCode ?? null);
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
  return (
    <div className="verification-fields">
      <p className="text-muted mb-4 text-xs">验证当前账号以确认此项操作</p>
      <div className="form-stack">
        <Notice>{error}</Notice>
        <Field
          label="当前账号密码"
          type="password"
          required
          value={password}
          onChange={onPassword}
          autoComplete="current-password"
        />
        {(totp || totp === null) && (
          <Field
            label="当前动态验证码"
            required
            value={code}
            onChange={onCode}
            maxLength={32}
            autoComplete="one-time-code"
          />
        )}
      </div>
    </div>
  );
}
