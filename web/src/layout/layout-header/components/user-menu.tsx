import type { ButtonProps } from "antd";
import { Avatar, Button, Dropdown, App } from "antd";
import { LogoutOutlined } from "@ant-design/icons";
import { useAuthStore } from "#src/store/auth";
import { useUserStore } from "#src/store/user";
export function UserMenu(props: ButtonProps) {
  const username = useUserStore((s) => s.username);
  const logout = useAuthStore((s) => s.logout);
  const { message } = App.useApp();
  return (
    <Dropdown
      trigger={["click"]}
      menu={{
        items: [{ key: "logout", label: "退出登录", icon: <LogoutOutlined /> }],
        onClick: () => {
          void logout().catch(() => message.error("退出失败，请重试"));
        },
      }}
    >
      <Button {...props} type="text" aria-label="账户菜单">
        <Avatar size="small">{username.slice(0, 1).toUpperCase()}</Avatar>
        <span>{username}</span>
      </Button>
    </Dropdown>
  );
}
