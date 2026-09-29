import { Bell } from "@gravity-ui/icons";
import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { Button, Popover } from "@heroui/react";
import { useConnectionRequests } from "../store/connection_requests";
import { RequestsPanel } from "../pages/identity/requests";
export function DashboardNavbar({ title }: { title: string }) {
  const requests = useConnectionRequests();
  return (
    <Navbar maxWidth="full">
      <Navbar.Header>
        <AppLayout.MenuToggle aria-label="打开菜单" />
        <Sidebar.Trigger aria-label="收起侧栏" />
        <h1 className="text-foreground min-w-0 break-words text-xl font-semibold">
          {title}
        </h1>
        <Navbar.Spacer />
        <Popover isOpen={requests.open} onOpenChange={requests.setOpen}>
          <Button
            isIconOnly
            size="sm"
            variant="tertiary"
            aria-label="查看连接申请"
          >
            <Bell className="size-4" />
          </Button>
          <Popover.Content
            placement="bottom end"
            className="w-[min(26rem,calc(100vw-2rem))] max-w-none"
          >
            <Popover.Dialog>
              <Popover.Heading>连接申请</Popover.Heading>
              <RequestsPanel />
            </Popover.Dialog>
          </Popover.Content>
        </Popover>
      </Navbar.Header>
    </Navbar>
  );
}
