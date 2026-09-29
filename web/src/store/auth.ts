import { create } from "zustand";
import {
  currentUser,
  login,
  logout,
} from "#src/pages/identity/identity.service";
import { useUserStore } from "./user";
import { useTabsStore } from "./tabs";
interface AuthState {
  authenticated: boolean;
  initialized: boolean;
  error: string;
  initialize: () => Promise<void>;
  login: (values: {
    username: string;
    password: string;
    code?: string;
  }) => Promise<void>;
  logout: () => Promise<void>;
}
let initialization: Promise<void> | undefined;
export const useAuthStore = create<AuthState>((set) => ({
  authenticated: false,
  initialized: false,
  error: "",
  initialize: () =>
    (initialization ??= (async () => {
      try {
        const user = await currentUser();
        useUserStore.setState({
          id: user?.id ?? "",
          username: user?.username ?? "",
          role: user?.role ?? "user",
        });
        set({ authenticated: !!user, initialized: true });
      } catch {
        set({ initialized: true, error: "无法连接平台，请重新加载。" });
      }
    })()),
  login: async (values) => {
    const user = await login(values.username, values.password, values.code);
    useUserStore.setState({
      id: user.id,
      username: user.username,
      role: user.role,
    });
    set({ authenticated: true, error: "" });
  },
  logout: async () => {
    await logout();
    useUserStore.getState().reset();
    useTabsStore.getState().resetTabs();
    set({ authenticated: false });
  },
}));
