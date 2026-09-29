import type { AppRouteRecordRaw } from "#src/router/types";
import ContainerLayout from "#src/layout/container-layout";
import { IdentityPage } from "#src/pages/identity";
import { SafetyOutlined } from "@ant-design/icons";
import { createElement } from "react";
const routes: AppRouteRecordRaw[] = [
  {
    path: "/identity",
    Component: ContainerLayout,
    handle: {
      title: "个人安全设置",
      hideInMenu: true,
      icon: createElement(SafetyOutlined),
    },
    children: [
      {
        index: true,
        Component: IdentityPage,
        handle: { title: "个人安全设置", keepAlive: false },
      },
    ],
  },
];
export default routes;
