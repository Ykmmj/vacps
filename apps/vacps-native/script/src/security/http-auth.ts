/**
 * HTTP surface auth policy for the native agent.
 *
 * Only `/health` is unauthenticated. All other routes require a valid
 * control-plane Ed25519 signature when CONTROL_PLANE_PUBLIC_KEY is set.
 * Without a key, requests are rejected unless ALLOW_INSECURE_NO_AUTH is set.
 */

/** Paths that skip signature verification (load balancer / process probes only). */
export const PUBLIC_HTTP_PATHS = new Set<string>(['/health']);

export function isPublicHttpPath(path: string): boolean {
  // Exact match only — do not treat prefixes as public.
  return PUBLIC_HTTP_PATHS.has(path);
}

export type AuthConfig = {
  CONTROL_PLANE_PUBLIC_KEY?: string | undefined;
  ALLOW_INSECURE_NO_AUTH?: boolean | undefined;
};

/**
 * Fail closed at boot when the control-plane public key is missing,
 * unless explicit insecure dev mode is enabled.
 */
export function assertControlPlaneAuthConfig(config: AuthConfig): void {
  if (config.CONTROL_PLANE_PUBLIC_KEY) return;
  if (config.ALLOW_INSECURE_NO_AUTH) return;
  throw new Error(
    'CONTROL_PLANE_PUBLIC_KEY is required. Set VACPS_ALLOW_INSECURE_NO_AUTH=1 only for local/dev tests.',
  );
}

/**
 * Whether preValidation may skip signature checks entirely
 * (insecure mode with no key).
 */
export function allowUnsignedWhenNoKey(config: AuthConfig): boolean {
  return !config.CONTROL_PLANE_PUBLIC_KEY && Boolean(config.ALLOW_INSECURE_NO_AUTH);
}
