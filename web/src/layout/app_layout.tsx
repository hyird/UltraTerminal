import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { Avatar, Button, Dropdown, Label, toast } from "@heroui/react";
import { Outlet, useLocation, useNavigate } from "react-router";
import { useAuth } from "../store/auth";
import { Icon } from "../components/icon";
import { DesktopWorkspace } from "../pages/desktop";
import { useDesktops } from "../store/desktops";
import { failureText } from "../components/form";
const workbench = [
  { path: "/workbench/devices", label: "我的设备", icon: "display" },
  { path: "/workbench/requests", label: "连接申请", icon: "list-check" },
];
const resources = [
  { path: "/console/assets", label: "设备清单", icon: "server" },
  { path: "/console/grants", label: "设备授权", icon: "shield-check" },
];
const identities = [
  { path: "/console/users", label: "用户管理", icon: "persons" },
  { path: "/console/settings", label: "平台设置", icon: "gear" },
];
export function ApplicationLayout() {
  const { user, signOut } = useAuth();
  const desktops = useDesktops();
  const { pathname } = useLocation();
  const navigate = useNavigate();
  if (!user) return null;
  const administration = pathname.startsWith("/console/");
  const personal = pathname === "/account/security";
  const title =
    [...workbench, ...resources, ...identities].find(
      (item) => item.path === pathname,
    )?.label ?? (personal ? "个人安全" : "远程桌面");
  const group = (label: string, items: typeof workbench) => (
    <Sidebar.Group>
      <Sidebar.GroupLabel>{label}</Sidebar.GroupLabel>
      <Sidebar.Menu aria-label={label}>
        {items.map((item) => (
          <Sidebar.MenuItem
            key={item.path}
            id={item.path}
            href={item.path}
            isCurrent={pathname === item.path}
            textValue={item.label}
          >
            <Sidebar.MenuIcon>
              <Icon name={item.icon} />
            </Sidebar.MenuIcon>
            <Sidebar.MenuLabel>{item.label}</Sidebar.MenuLabel>
          </Sidebar.MenuItem>
        ))}
      </Sidebar.Menu>
    </Sidebar.Group>
  );
  const content = (
    <>
      <Sidebar.Header>
        <div className="brand">
          <span className="brand-mark">
            <Icon name="terminal" />
          </span>
          <span data-sidebar="label">Ultraterimal</span>
        </div>
        {user.role === "admin" && (
          <Dropdown>
            <Button
              variant="secondary"
              className="workspace-switch"
              aria-label="切换工作区"
            >
              <span className="text-left">
                {administration ? "管理控制台" : "工作台"}
              </span>
              <Icon name="chevron-down" className="size-4 text-muted" />
            </Button>
            <Dropdown.Popover>
              <Dropdown.Menu
                onAction={(key) =>
                  navigate(
                    key === "console"
                      ? "/console/assets"
                      : "/workbench/devices",
                  )
                }
              >
                <Dropdown.Item id="workbench" textValue="工作台">
                  <Icon name="display" />
                  <Label>工作台</Label>
                </Dropdown.Item>
                <Dropdown.Item id="console" textValue="管理控制台">
                  <Icon name="shield-check" />
                  <Label>管理控制台</Label>
                </Dropdown.Item>
              </Dropdown.Menu>
            </Dropdown.Popover>
          </Dropdown>
        )}
      </Sidebar.Header>
      <Sidebar.Content>
        {administration ? (
          <>
            {group("资源管理", resources)}
            {group("身份与平台", identities)}
          </>
        ) : (
          group(
            "访问工作台",
            desktops.entries.length
              ? [
                  ...workbench,
                  {
                    path: "/workbench/sessions",
                    label: "远程桌面",
                    icon: "display",
                  },
                ]
              : workbench,
          )
        )}
      </Sidebar.Content>
      <Sidebar.Footer>
        <Dropdown>
          <Button
            variant="ghost"
            className="account-trigger"
            aria-label="账户菜单"
          >
            <Avatar size="sm">
              <Avatar.Fallback>
                {user.username.slice(0, 1).toUpperCase()}
              </Avatar.Fallback>
            </Avatar>
            <span className="account-name" data-sidebar="label">
              <strong>{user.username}</strong>
              <small>{user.role === "admin" ? "管理员" : "普通用户"}</small>
            </span>
            <Icon name="chevron-down" className="size-4 text-muted" />
          </Button>
          <Dropdown.Popover placement="top start">
            <Dropdown.Menu
              onAction={(key) => {
                if (key === "security") navigate("/account/security");
                else
                  void signOut().catch((error) =>
                    toast.danger(failureText(error)),
                  );
              }}
            >
              <Dropdown.Item id="security" textValue="个人安全">
                <Icon name="lock" />
                <Label>个人安全</Label>
              </Dropdown.Item>
              <Dropdown.Item id="logout" textValue="退出登录">
                <Icon name="arrow-right-from-square" />
                <Label>退出登录</Label>
              </Dropdown.Item>
            </Dropdown.Menu>
          </Dropdown.Popover>
        </Dropdown>
      </Sidebar.Footer>
    </>
  );
  return (
    <AppLayout
      className="platform-layout"
      navigate={navigate}
      toggleShortcut={false}
      sidebarCollapsible="offcanvas"
      sidebar={
        <>
          <Sidebar>{content}</Sidebar>
          <Sidebar.Mobile>{content}</Sidebar.Mobile>
        </>
      }
      navbar={
        <Navbar maxWidth="full">
          <Navbar.Header>
            <AppLayout.MenuToggle aria-label="打开菜单" />
            <span className="text-muted text-sm">
              {personal ? "账号" : administration ? "管理控制台" : "工作台"}
              <span className="mx-3 text-muted/50">/</span>
              <span className="text-foreground font-medium">{title}</span>
            </span>
          </Navbar.Header>
        </Navbar>
      }
    >
      <Outlet />
      <DesktopWorkspace />
    </AppLayout>
  );
}
