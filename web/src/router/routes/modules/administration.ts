import type { AppRouteRecordRaw } from "#src/router/types";
import ContainerLayout from "#src/layout/container-layout";
import { AccountsPage } from "#src/pages/identity/accounts";
import { TelegramSettingsPage } from "#src/pages/identity/telegram";
import { DeviceAssignmentsPage } from "#src/pages/devices/assignments";
import {
  SettingOutlined,
  TeamOutlined,
  DesktopOutlined,
  SendOutlined,
} from "@ant-design/icons";
import { createElement } from "react";

const routes: AppRouteRecordRaw[] = [
  {
    path: "/administration",
    Component: ContainerLayout,
    handle: {
      title: "平台管理",
      icon: createElement(SettingOutlined),
      roles: ["admin"],
      order: 20,
    },
    children: [
      {
        path: "/administration/accounts",
        Component: AccountsPage,
        handle: {
          title: "账号管理",
          icon: createElement(TeamOutlined),
          roles: ["admin"],
          keepAlive: false,
        },
      },
      {
        path: "/administration/devices",
        Component: DeviceAssignmentsPage,
        handle: {
          title: "机器分配",
          icon: createElement(DesktopOutlined),
          roles: ["admin"],
          keepAlive: false,
        },
      },
      {
        path: "/administration/telegram",
        Component: TelegramSettingsPage,
        handle: {
          title: "Telegram Bot 配置",
          icon: createElement(SendOutlined),
          roles: ["admin"],
          keepAlive: false,
        },
      },
    ],
  },
];
export default routes;
