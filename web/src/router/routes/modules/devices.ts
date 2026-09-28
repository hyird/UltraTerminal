import type { AppRouteRecordRaw } from "#src/router/types";
import ContainerLayout from "#src/layout/container-layout";
import { DevicesPage } from "#src/pages/devices";
import { DesktopOutlined } from "@ant-design/icons";
import { createElement } from "react";
const routes: AppRouteRecordRaw[] = [
  {
    path: "/devices",
    Component: ContainerLayout,
    handle: { title: "我的设备", icon: createElement(DesktopOutlined) },
    children: [
      { index: true, Component: DevicesPage, handle: { title: "我的设备" } },
    ],
  },
];
export default routes;
