import { useEffect } from "react";
import { Grid } from "antd";
import { usePreferencesStore } from "#src/store/preferences";
import { useTabsStore } from "#src/store/tabs";
import { useLayoutHeaderStyle } from "#src/hooks/use-layout-style";
import LayoutHeader from "../layout-header";
import LayoutSidebar from "../layout-sidebar";
import LayoutMenu from "../layout-menu";
import { useMenu } from "../layout-menu/use-menu";
import LayoutContent from "../layout-content";
import LayoutMobileMenu from "../layout-mobile-menu";
import { BreadcrumbViews } from "../widgets/breadcrumb-views";
import { headerHeight, tabbarHeight } from "../constants";
import { WorkspaceSwitcher } from "../workspace-switcher";
// Upstream ContainerLayout, trimmed to side navigation and the real workspace.
export default function ContainerLayout() {
  const screens = Grid.useBreakpoint();
  const { sidebarWidth, sideCollapsedWidth, sidebarCollapsed, setPreferences } =
    usePreferencesStore();
  const isMaximize = useTabsStore((s) => s.isMaximize);
  const { sideNavItems, handleMenuSelect } = useMenu();
  const { setLayoutHeaderHeight } = useLayoutHeaderStyle();
  const isMobile = !screens.md;
  useEffect(() => {
    if (screens.lg && !screens.xl) setPreferences("sidebarCollapsed", true);
    else if (screens.xl) setPreferences("sidebarCollapsed", false);
  }, [screens.lg, screens.xl, setPreferences]);
  useEffect(() => {
    setLayoutHeaderHeight(isMaximize ? 0 : headerHeight + tabbarHeight);
  }, [isMaximize, setLayoutHeaderHeight]);
  const computedSidebarWidth =
    isMaximize || isMobile
      ? 0
      : sidebarCollapsed
        ? sideCollapsedWidth
        : sidebarWidth;
  return (
    <section
      style={{ paddingLeft: computedSidebarWidth }}
      className="flex flex-col h-dvh min-h-0 overflow-hidden"
    >
      <LayoutHeader>
        <WorkspaceSwitcher />
        {!isMobile && <BreadcrumbViews />}
      </LayoutHeader>
      <LayoutMobileMenu />
      {!isMaximize && !isMobile && (
        <LayoutSidebar computedSidebarWidth={computedSidebarWidth}>
          <LayoutMenu
            autoExpandCurrentMenu
            menus={sideNavItems}
            handleMenuSelect={handleMenuSelect}
          />
        </LayoutSidebar>
      )}
      <LayoutContent />
    </section>
  );
}
