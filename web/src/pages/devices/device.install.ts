import type { Enrollment } from "./device.service";

export function onboardingCommand(enrollment: Enrollment): string {
  const binding = enrollment.code
    ? `code=${enrollment.code}`
    : `device=${enrollment.deviceId}`;
  return `powershell -NoProfile -c "irm '${enrollment.webOrigin}/api/install.ps1?${binding}' | iex"`;
}
