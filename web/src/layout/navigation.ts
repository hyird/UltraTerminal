import {
  Display,
  Gear,
  LayoutHeaderCellsLarge,
  Persons,
  ShieldCheck,
} from "@gravity-ui/icons";
export const navigation = [
  {
    href: "/overview",
    label: "概览",
    icon: LayoutHeaderCellsLarge,
    admin: false,
  },
  {
    href: "/workbench/devices",
    label: "我的设备",
    icon: Display,
    admin: false,
  },
  {
    href: "/console/grants",
    label: "设备授权",
    icon: ShieldCheck,
    admin: true,
  },
  { href: "/console/users", label: "用户管理", icon: Persons, admin: true },
  { href: "/console/settings", label: "平台设置", icon: Gear, admin: true },
];
