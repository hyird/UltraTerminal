import {
  Alert,
  Button,
  Description,
  FieldError,
  Input,
  Label,
  ListBox,
  Modal,
  Select,
  Switch,
  TextField,
} from "@heroui/react";
import type { ReactNode } from "react";

export function Field({
  label,
  value,
  onChange,
  type = "text",
  required = false,
  disabled = false,
  hint,
  autoComplete,
  maxLength,
  minLength,
  pattern,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  type?: "text" | "password";
  required?: boolean;
  disabled?: boolean;
  hint?: string;
  autoComplete?: string;
  maxLength?: number;
  minLength?: number;
  pattern?: string;
}) {
  return (
    <TextField
      value={value}
      onChange={onChange}
      type={type}
      isRequired={required}
      isDisabled={disabled}
      autoComplete={autoComplete}
      className="w-full"
      variant="secondary"
      minLength={minLength}
      maxLength={maxLength}
      pattern={pattern}
    >
      <Label>{label}</Label>
      <Input className="w-full" />
      {hint && <Description>{hint}</Description>}
      <FieldError />
    </TextField>
  );
}
export function Choice({
  label,
  value,
  onChange,
  items,
  disabled = false,
  compact = false,
  popoverClassName,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  items: { id: string; label: string }[];
  disabled?: boolean;
  compact?: boolean;
  popoverClassName?: string;
}) {
  return (
    <Select
      aria-label={label}
      value={value || null}
      onChange={(key) => {
        if (typeof key === "string") onChange(key);
      }}
      isDisabled={disabled}
      placeholder="请选择"
      className={compact ? "min-w-32 max-w-56" : "w-full"}
      variant="secondary"
    >
      {!compact && <Label>{label}</Label>}
      <Select.Trigger>
        <Select.Value />
        <Select.Indicator />
      </Select.Trigger>
      <Select.Popover className={popoverClassName}>
        <ListBox items={items}>
          {(item) => (
            <ListBox.Item id={item.id} textValue={item.label}>
              {item.label}
              <ListBox.ItemIndicator />
            </ListBox.Item>
          )}
        </ListBox>
      </Select.Popover>
    </Select>
  );
}
export function Toggle({
  label,
  hint,
  selected,
  onChange,
  disabled = false,
}: {
  label: string;
  hint?: string;
  selected: boolean;
  onChange: (value: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <div className="flex items-center justify-between gap-5 py-2">
      <div>
        <div className="text-sm font-medium">{label}</div>
        {hint && <p className="text-muted mt-1 text-xs">{hint}</p>}
      </div>
      <Switch
        size="sm"
        aria-label={label}
        isSelected={selected}
        onChange={onChange}
        isDisabled={disabled}
      >
        <Switch.Content>
          <Switch.Control>
            <Switch.Thumb />
          </Switch.Control>
        </Switch.Content>
      </Switch>
    </div>
  );
}
export function Notice({
  children,
  success = false,
}: {
  children: ReactNode;
  success?: boolean;
}) {
  return children ? (
    <Alert status={success ? "success" : "danger"}>
      <Alert.Indicator />
      <Alert.Content>
        <Alert.Description>{children}</Alert.Description>
      </Alert.Content>
    </Alert>
  ) : null;
}
export function Dialog({
  title,
  children,
  onClose,
  busy = false,
}: {
  title: string;
  children: ReactNode;
  onClose: () => void;
  busy?: boolean;
}) {
  return (
    <Modal.Backdrop
      isOpen
      onOpenChange={(open) => {
        if (!open && !busy) onClose();
      }}
      isDismissable={!busy}
      isKeyboardDismissDisabled={busy}
    >
      <Modal.Container size="sm" placement="center" scroll="inside">
        <Modal.Dialog>
          <Modal.CloseTrigger isDisabled={busy} />
          <Modal.Header>
            <Modal.Heading>{title}</Modal.Heading>
          </Modal.Header>
          <Modal.Body className="pb-4">{children}</Modal.Body>
        </Modal.Dialog>
      </Modal.Container>
    </Modal.Backdrop>
  );
}
export function FormActions({
  onCancel,
  busy,
  label = "保存",
  danger = false,
}: {
  onCancel: () => void;
  busy: boolean;
  label?: string;
  danger?: boolean;
}) {
  return (
    <div className="flex justify-end gap-2 pt-2">
      <Button size="sm" variant="tertiary" onPress={onCancel} isDisabled={busy}>
        取消
      </Button>
      <Button
        size="sm"
        type="submit"
        isPending={busy}
        variant={danger ? "danger-soft" : "primary"}
      >
        {label}
      </Button>
    </div>
  );
}
export function failureText(error: unknown): string {
  return error instanceof Error ? error.message : "操作未完成，请稍后重试";
}
