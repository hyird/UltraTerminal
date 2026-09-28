export interface DesktopGrant {
  id: string;
  purpose: 2 | 3;
  ticket: string;
}
export interface DesktopTicket {
  id: string;
  writeControl: boolean;
  channels: DesktopGrant[];
}
