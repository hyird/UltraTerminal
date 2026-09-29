import { create } from "zustand";
export const useUserStore = create<{
  id: string;
  username: string;
  role: "admin" | "user";
  reset: () => void;
}>((set) => ({
  id: "",
  username: "",
  role: "user",
  reset: () => set({ id: "", username: "", role: "user" }),
}));
