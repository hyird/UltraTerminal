import { create } from "zustand";
export const useUserStore = create<{
  id: string;
  username: string;
  reset: () => void;
}>((set) => ({
  id: "",
  username: "",
  reset: () => set({ id: "", username: "" }),
}));
