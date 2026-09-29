import { usePreferences } from "#src/hooks/use-preferences";
import type { ButtonProps } from "antd";
import { useDeviceType } from "#src/hooks/use-device-type";
import { useLayout } from "#src/layout/hooks/use-layout";
import { useTabsStore } from "#src/store/tabs";
import { cn } from "#src/utils/cn";

import { MenuFoldOutlined, MenuUnfoldOutlined } from "@ant-design/icons";
import { theme as antdTheme, Button, ConfigProvider, theme } from "antd";

import { headerHeight } from "../constants";
import { FullscreenButton } from "./components/fullscreen-button";
import { UserMenu } from "./components/user-menu";

export interface LayoutHeaderProps {
  className?: string;
  children?: React.ReactNode;
}

const buttonProps: ButtonProps = {
  size: "large",
  className: "px-[11px]",
};

export default function LayoutHeader({
  className,
  children,
}: LayoutHeaderProps) {
  const {
    token: { Menu },
  } = theme.useToken();
  const { sidebarCollapsed, isDark, sidebarTheme, setPreferences } =
    usePreferences();
  const { isMobile } = useDeviceType();
  const isMaximize = useTabsStore((state) => state.isMaximize);
  const { isTopNav, isMixedNav } = useLayout();
  const isFixedDarkTheme =
    isDark || (sidebarTheme === "dark" && (isMixedNav || isTopNav));

  return (
    <ConfigProvider
      theme={{
        algorithm: isFixedDarkTheme
          ? antdTheme.darkAlgorithm
          : antdTheme.defaultAlgorithm,
      }}
    >
      <header
        className={cn(
          "flex-shrink-0 flex gap-2 md:gap-5 justify-between items-center md:px-4",
          { "overflow-hidden": isMaximize },
          className,
        )}
        style={{
          background: isFixedDarkTheme ? Menu?.darkItemBg : Menu?.itemBg,
          height: isMaximize ? 0 : headerHeight,
        }}
      >
        {isMobile ? (
          <Button
            type="text"
            icon={
              sidebarCollapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />
            }
            onClick={() =>
              setPreferences("sidebarCollapsed", !sidebarCollapsed)
            }
            aria-label="切换导航"
            className="h-full"
          />
        ) : null}

        <div className="flex items-center grow h-full overflow-hidden">
          {children}
        </div>

        <div className="flex items-center">
          <FullscreenButton
            {...buttonProps}
            target={document.documentElement}
          />
          <UserMenu {...buttonProps} />
        </div>
      </header>
    </ConfigProvider>
  );
}
