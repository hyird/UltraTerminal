import { Bell } from "@gravity-ui/icons";
import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { useNavigate } from "react-router";
import { IconButton } from "../components/icon_button";
export function DashboardNavbar({ title }: { title: string }) {
  const navigate = useNavigate();
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
        </div>
      </Navbar.Header>
    </Navbar>
  );
}
