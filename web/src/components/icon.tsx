import {
  ChevronDown,
  Display,
  Ellipsis,
  FileText,
  Key,
  PaperPlane,
  Plus,
  ShieldCheck,
  Terminal,
} from "@gravity-ui/icons";
const icons = {
  "chevron-down": ChevronDown,
  display: Display,
  ellipsis: Ellipsis,
  "file-text": FileText,
  key: Key,
  "paper-plane": PaperPlane,
  plus: Plus,
  "shield-check": ShieldCheck,
  terminal: Terminal,
};
export type IconName = keyof typeof icons;
export function Icon({
  name,
  className = "size-5",
}: {
  name: IconName;
  className?: string;
}) {
  const Glyph = icons[name];
  return <Glyph className={className} aria-hidden="true" />;
}
