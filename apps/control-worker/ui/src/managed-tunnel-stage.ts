export type ManagedTunnelStage =
  'unconfigured' | 'needs_connect' | 'needs_zone' | 'needs_tunnel' | 'ready';

export interface ManagedTunnelStatusInput {
  configured?: boolean;
  connected?: boolean;
  zoneId?: string;
  baseDomain?: string;
}

/** Pure stage derivation for the Managed Tunnel install wizard. */
export function deriveManagedTunnelStage(
  status: ManagedTunnelStatusInput | undefined,
  managedProvision: unknown,
): ManagedTunnelStage {
  if (!status?.configured) return 'unconfigured';
  if (!status.connected) return 'needs_connect';
  if (!status.zoneId || !status.baseDomain) return 'needs_zone';
  if (!managedProvision) return 'needs_tunnel';
  return 'ready';
}

/** Progress-rail index matching the existing 0–4 steps in InstallComposer. */
export function managedTunnelStageIndex(stage: ManagedTunnelStage): number {
  switch (stage) {
    case 'unconfigured':
      return 0;
    case 'needs_connect':
      return 1;
    case 'needs_zone':
      return 2;
    case 'needs_tunnel':
      return 3;
    case 'ready':
      return 4;
  }
}
