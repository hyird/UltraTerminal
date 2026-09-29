import type { ButtonProps } from "antd";
import { Avatar, Button, Dropdown, App } from "antd";
import { LogoutOutlined, SafetyOutlined } from "@ant-design/icons";
import { useNavigate } from "react-router";
import { useAuthStore } from "#src/store/auth";
import { useUserStore } from "#src/store/user";
export function UserMenu(props: ButtonProps) {
  const username = useUserStore((s) => s.username);
  const logout = useAuthStore((s) => s.logout);
  const { message } = App.useApp();
  const navigate = useNavigate();
  return (
    <Dropdown
      trigger={["click"]}
      menu={{
        items: [
          { key: "security", label: "个人安全设置", icon: <SafetyOutlined /> },
          { type: "divider" },
          { key: "logout", label: "退出登录", icon: <LogoutOutlined /> },
        ],
        onClick: ({ key }) => {
          if (key === "security") void navigate("/identity");
          else if (key === "logout")
            void logout().catch(() => message.error("退出失败，请重试"));
        },
      }}
    >
      <Button {...props} type="text" aria-label="账户菜单">
        <Avatar size="small">{username.slice(0, 1).toUpperCase()}</Avatar>
        <span className="hidden md:inline">{username}</span>
      </Button>
    </Dropdown>
  );
}
