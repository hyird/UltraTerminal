import { Bell } from "@gravity-ui/icons";
import { AppLayout, Navbar, Sidebar } from "@heroui-pro/react";
import { Button, Popover } from "@heroui/react";
import { useConnectionRequests } from "../store/connection_requests";
import { RequestsPanel } from "../pages/identity/requests";
import type { ReactNode, Ref } from "react";
export function DashboardNavbar({
  title,
  children,
  headingRef,
}: {
  title: string;
  children?: ReactNode;
  headingRef?: Ref<HTMLHeadingElement>;
}) {
  const requests = useConnectionRequests();
  return (
    <Navbar maxWidth="full">
      <Navbar.Header className="min-w-0 gap-3 max-sm:gap-2">
        <AppLayout.MenuToggle aria-label="打开菜单" />
        <Sidebar.Trigger aria-label="收起侧栏" />
        <h1
          ref={headingRef}
          tabIndex={-1}
          className={`text-foreground min-w-0 text-xl font-semibold focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-focus ${children ? "shrink-0 whitespace-nowrap max-sm:sr-only" : "break-words"}`}
        >
          {title}
        </h1>
        {children ? (
          <div className="min-w-0 flex-1 overflow-x-auto [scrollbar-width:none] [&::-webkit-scrollbar]:hidden">
            {children}
          </div>
        ) : (
          <Navbar.Spacer />
        )}
        <Popover isOpen={requests.open} onOpenChange={requests.setOpen}>
          <Button
            isIconOnly
            variant="tertiary"
            className="shrink-0"
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
