import { Bell, Display } from "@gravity-ui/icons";
import { Button } from "@heroui/react";
import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { useNavigate } from "react-router";
import { IconButton } from "../components/icon_button";
import { useAuth } from "../store/auth";
export function DashboardNavbar({ title }: { title: string }) {
  const navigate = useNavigate();
  const { user } = useAuth();
  return (
    <Navbar maxWidth="full">
      <Navbar.Header>
        <AppLayout.MenuToggle aria-label="打开菜单" />
        <Sidebar.Trigger aria-label="收起侧栏" />
        <h1 className="text-foreground min-w-0 break-words text-xl font-semibold">
          {title}
        </h1>
        <Navbar.Spacer />
        <div className="flex items-center gap-2">
          <IconButton
            label="查看连接申请"
            size="sm"
            variant="tertiary"
            onPress={() => navigate("/workbench/requests")}
          >
            <Bell className="size-4" />
          </IconButton>
          <Button
            size="sm"
            onPress={() =>
              navigate(
                user?.role === "admin"
                  ? "/console/assets"
                  : "/workbench/devices",
              )
            }
          >
            <Display className="size-4" />
            <span className="hidden sm:inline">
              {user?.role === "admin" ? "管理设备" : "访问设备"}
            </span>
          </Button>
        </div>
      </Navbar.Header>
    </Navbar>
  );
}
