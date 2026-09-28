import { Alert, Button, Form, Input } from "antd";
import { useState } from "react";
import { useAuthStore } from "#src/store/auth";
export function PasswordLogin() {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const login = useAuthStore((s) => s.login);
  async function submit(values: { username: string; password: string }) {
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await login(values);
    } catch {
      setError("登录失败，请检查用户名和密码或网络连接");
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <h2 className="text-colorText mb-3 text-3xl font-bold leading-9 tracking-tight lg:text-4xl">
        登录工作台
      </h2>
      <p className="mb-6 text-colorTextSecondary">
        使用平台账户访问已授权设备。
      </p>
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
        {error && (
          <Alert role="alert" className="mb-4" type="error" title={error} />
        )}
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
    </>
  );
}
