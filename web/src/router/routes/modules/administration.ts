import type { AppRouteRecordRaw } from "#src/router/types";
import ContainerLayout from "#src/layout/container-layout";
import { AccountsPage } from "#src/pages/identity/accounts";
import { TelegramSettingsPage } from "#src/pages/identity/telegram";
import { DeviceAssignmentsPage } from "#src/pages/devices/assignments";
import {
  BellOutlined,
  TeamOutlined,
  ShareAltOutlined,
} from "@ant-design/icons";
import { createElement } from "react";

const routes: AppRouteRecordRaw[] = [
  {
    path: "/administration/devices",
    Component: ContainerLayout,
    handle: {
      title: "设备授权",
      icon: createElement(ShareAltOutlined),
      roles: ["admin"],
      order: 10,
    },
    children: [
      {
        index: true,
        Component: DeviceAssignmentsPage,
        handle: { title: "设备授权", keepAlive: false },
      },
    ],
  },
  {
    path: "/administration/accounts",
    Component: ContainerLayout,
    handle: {
      title: "用户管理",
      icon: createElement(TeamOutlined),
      roles: ["admin"],
      order: 20,
    },
    children: [
      {
        index: true,
        Component: AccountsPage,
        handle: { title: "用户管理", keepAlive: false },
      },
    ],
  },
  {
    path: "/administration/telegram",
    Component: ContainerLayout,
    handle: {
      title: "系统设置",
      icon: createElement(BellOutlined),
      roles: ["admin"],
      order: 30,
    },
    children: [
      {
        index: true,
        Component: TelegramSettingsPage,
        handle: { title: "系统设置", keepAlive: false },
      },
    ],
  },
];
export default routes;
