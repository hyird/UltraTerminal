import { Bell, Display, Gear } from "@gravity-ui/icons";
import { Button } from "@heroui/react";
import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { useLocation, useNavigate } from "react-router";
import { IconButton } from "../components/icon_button";
import { useAuth } from "../store/auth";
export function DashboardNavbar({ title }: { title: string }) {
  const navigate = useNavigate();
  const { user } = useAuth();
  const { pathname } = useLocation();
  const administration = pathname.startsWith("/console/");
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
          {user?.role === "admin" && (
            <Button
              aria-label={administration ? "返回工作台" : "管理控制台"}
              size="sm"
              variant="secondary"
              onPress={() =>
                navigate(administration ? "/overview" : "/console/assets")
              }
            >
              {administration ? (
                <Display className="size-4" />
              ) : (
                <Gear className="size-4" />
              )}
              <span>{administration ? "工作台" : "管理控制台"}</span>
            </Button>
          )}
        </div>
      </Navbar.Header>
    </Navbar>
  );
}
