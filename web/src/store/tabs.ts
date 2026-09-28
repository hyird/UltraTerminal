import { create } from "zustand";
interface TabsState {
  isMaximize: boolean;
  toggleMaximize: (state: boolean) => void;
  resetTabs: () => void;
}
// Layout presentation only; device tabs and live sessions belong to the workspace.
export const useTabsStore = create<TabsState>((set) => ({
  isMaximize: false,
  toggleMaximize: (isMaximize) => set({ isMaximize }),
  resetTabs: () => set({ isMaximize: false }),
}));
