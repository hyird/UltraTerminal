import { Alert, Button, Form, Input } from "antd";
import { useState } from "react";
import { ApiError } from "#src/lib/http";
import { useAuthStore } from "#src/store/auth";

interface Credentials {
  username: string;
  password: string;
}
export function PasswordLogin() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [credentials, setCredentials] = useState<Credentials | null>(null);
  const [recovering, setRecovering] = useState(false);
  const [factorForm] = Form.useForm<{ code: string }>();
  const login = useAuthStore((s) => s.login);
  async function submit(values: Credentials & { code?: string }) {
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await login(values);
      setCredentials(null);
      factorForm.resetFields();
    } catch (failure) {
      if (failure instanceof ApiError && failure.code === "factor_required") {
        setCredentials({
          username: values.username,
          password: values.password,
        });
      } else
        setError(
          credentials
            ? "验证失败，请重试；多次失败后请等待 5 分钟"
            : "登录失败，请检查用户名和密码；多次失败后请等待 5 分钟",
        );
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <h2 className="text-colorText mb-3 text-3xl font-bold leading-9 tracking-tight lg:text-4xl">
        {credentials
          ? recovering
            ? "恢复账号访问"
            : "验证身份"
          : "登录工作台"}
      </h2>
      <p className="mb-6 text-colorTextSecondary">
        {credentials
          ? recovering
            ? "使用绑定验证器时保存的一次性恢复码。"
            : "请输入 Google 验证器显示的六位动态码。"
          : "使用平台账户访问已授权设备。"}
      </p>
      {error && (
        <Alert role="alert" className="mb-4" type="error" title={error} />
      )}
      {credentials ? (
        <Form
          form={factorForm}
          layout="vertical"
          onFinish={({ code }) => void submit({ ...credentials, code })}
        >
          <Form.Item
            name="code"
            label={recovering ? "恢复码" : "动态验证码"}
            rules={[
              { required: true },
              {
                pattern: recovering ? /^[a-f0-9]{32}$/ : /^\d{6}$/,
                message: recovering ? "请输入 32 位恢复码" : "请输入六位验证码",
              },
            ]}
          >
            <Input
              autoFocus
              autoComplete={recovering ? "off" : "one-time-code"}
              inputMode={recovering ? "text" : "numeric"}
              maxLength={recovering ? 32 : 6}
              size="large"
            />
          </Form.Item>
          <Button
            type="primary"
            htmlType="submit"
            block
            loading={busy}
            size="large"
          >
            验证并登录
          </Button>
          <Button
            type="link"
            disabled={busy}
            onClick={() => {
              setRecovering(!recovering);
              factorForm.resetFields();
              setError("");
            }}
          >
            {recovering ? "使用验证器" : "无法使用验证器"}
          </Button>
          <Button
            type="link"
            disabled={busy}
            onClick={() => {
              setCredentials(null);
              setRecovering(false);
              factorForm.resetFields();
              setError("");
            }}
          >
            返回登录
          </Button>
        </Form>
      ) : (
        <Form layout="vertical" onFinish={submit}>
          <Form.Item
            name="username"
            label="用户名"
            rules={[{ required: true, message: "请输入用户名" }]}
          >
            <Input
              autoFocus
              autoComplete="username"
              maxLength={64}
              size="large"
            />
          </Form.Item>
          <Form.Item
            name="password"
            label="密码"
            rules={[{ required: true, message: "请输入密码" }]}
          >
            <Input.Password
              autoComplete="current-password"
              maxLength={1024}
              size="large"
            />
          </Form.Item>
          <Button
            type="primary"
            htmlType="submit"
            block
            loading={busy}
            size="large"
          >
            登录
          </Button>
        </Form>
      )}
    </>
  );
}
