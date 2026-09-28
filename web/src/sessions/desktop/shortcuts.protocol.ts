export const desktopShortcuts = [
  { id: "secure-attention", label: "Ctrl+Alt+Del", keys: [] },
  {
    id: "task-manager",
    label: "Ctrl+Shift+Esc",
    keys: ["ControlLeft", "ShiftLeft", "Escape"],
  },
  { id: "run", label: "Win+R", keys: ["MetaLeft", "KeyR"] },
  { id: "desktop", label: "Win+D", keys: ["MetaLeft", "KeyD"] },
  {
    id: "explorer",
    label: "Win+E",
    keys: ["MetaLeft", "KeyE"],
  },
  {
    id: "switch-window",
    label: "Alt+Tab",
    keys: ["AltLeft", "Tab"],
  },
  { id: "close-window", label: "Alt+F4", keys: ["AltLeft", "F4"] },
  { id: "escape", label: "Esc", keys: ["Escape"] },
] as const;
