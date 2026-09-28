import { Alert, Button, Form, Input, Modal } from "antd";
import { useEffect, useRef, useState } from "react";
import type { Device } from "./device.types";
import { windowsLogin } from "./device.service";

export function WindowsLogin({
  device,
  close,
}: {
  device: Device;
  close: () => void;
}) {
  const [form] = Form.useForm<{ username?: string; password?: string }>();
  const [busy, setBusy] = useState(true);
  const [ready, setReady] = useState(false);
  const [saved, setSaved] = useState(false);
  const [username, setUsername] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    let cancelled = false;
    void windowsLogin(device.id, "read")
      .then((value) => {
        if (cancelled) return;
        setSaved(value.saved);
        setUsername(value.username);
        setReady(true);
      })
      .catch(() => {
        if (!cancelled)
          setError("无法读取 Agent 中的凭据状态，请稍后重新打开。");
      })
      .finally(() => {
        if (!cancelled) setBusy(false);
      });
    return () => {
      cancelled = true;
      alive.current = false;
      form.resetFields();
    };
  }, [device.id, form]);
  async function save() {
    if (busy || !ready) return;
    const values = await form.validateFields().catch(() => undefined);
    if (!values) return;
    const account = {
      username: values.username ?? "",
      password: values.password ?? "",
      domain: "",
    };
    if (!account.username && !account.password) {
      close();
      return;
    }
    if (!saved && (!account.username || !account.password)) {
      setError("首次保存请填写用户名和密码。");
      return;
    }
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const result = await windowsLogin(device.id, "save", account);
      if (!alive.current) return;
      setSaved(result.saved);
      setUsername(result.username);
      form.resetFields();
      setNotice("凭据已加密保存到这台 Agent。");
    } catch {
      if (alive.current) {
        setReady(false);
        setError("操作未确认完成。请重新打开此窗口核对 Agent 状态后再操作。");
      }
    } finally {
      account.password = "";
      values.password = "";
      form.setFieldValue("password", "");
      if (alive.current) setBusy(false);
    }
  }
  return (
    <Modal
      open
      title={`Windows 凭据 · ${device.name}`}
      onCancel={close}
      destroyOnHidden
      footer={
        <Button
          type="primary"
          disabled={!ready}
          loading={busy}
          onClick={() => void save()}
        >
          保存
        </Button>
      }
    >
      {error && <Alert className="mb-3" type="error" title={error} />}
      {notice && <Alert className="mb-3" type="success" title={notice} />}
      <Form
        form={form}
        layout="vertical"
        disabled={busy || !ready}
        autoComplete="off"
      >
        <Form.Item name="username" label="Windows 用户名">
          <Input
            maxLength={256}
            autoComplete="off"
            placeholder={saved ? "留空保持现有用户名" : "请输入用户名"}
          />
        </Form.Item>
        <Form.Item name="password" label="Windows 密码">
          <Input.Password
            maxLength={512}
            autoComplete="new-password"
            placeholder={saved ? "留空保持现有密码" : "请输入密码"}
          />
        </Form.Item>
      </Form>
    </Modal>
  );
}
