import { Icon as Iconify, addCollection } from "@iconify/react";
import icons from "@iconify-json/gravity-ui/icons.json";
addCollection(icons);
export function Icon({
  name,
  className = "size-5",
}: {
  name: string;
  className?: string;
}) {
  return (
    <Iconify
      icon={`gravity-ui:${name}`}
      className={className}
      aria-hidden="true"
    />
  );
}
