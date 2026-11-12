import {
  Display,
  Ellipsis,
  FileText,
  Key,
  PaperPlane,
  Plus,
  ShieldCheck,
} from "@gravity-ui/icons";
const icons = {
  display: Display,
  ellipsis: Ellipsis,
  "file-text": FileText,
  key: Key,
  "paper-plane": PaperPlane,
  plus: Plus,
  "shield-check": ShieldCheck,
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
