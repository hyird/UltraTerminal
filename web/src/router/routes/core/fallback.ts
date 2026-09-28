import type { AppRouteRecordRaw } from "#src/router/types";

import { lazy } from "react";

const NotFound = lazy(() => import("#src/pages/exception/404"));

const routes: AppRouteRecordRaw[] = [
  {
    path: "*",
    Component: NotFound,
    handle: {
      title: "404",
      hideInMenu: true,
    },
  },
];

export default routes;
