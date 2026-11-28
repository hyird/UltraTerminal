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
  Volume,
  VolumeXmark,
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
  volume: Volume,
  "volume-off": VolumeXmark,
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
