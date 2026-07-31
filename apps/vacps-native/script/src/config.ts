import * as host from 'vacps:host';

/**
 * Agent config from process env (parity with apps/vacps/src/config.ts).
 * Unset CONTROL_PLANE_URL disables registration/telemetry.
 */
export interface AgentConfig {
  BACKEND_ID: string;
  BACKEND_NAME: string;
  BACKEND_TAGS: string[];
  /** Inbound bind (passed to new http.Server). Not owned by C++ Config. */
  LISTEN_HOST: string;
  LISTEN_PORT: number;
  PUBLIC_BASE_URL: string | undefined;
  CONTROL_PLANE_URL: string | undefined;
  AGENT_PRIVATE_KEY: string | undefined;
  AGENT_PUBLIC_KEY: string | undefined;
  CONTROL_PLANE_PUBLIC_KEY: string | undefined;
  /**
   * When true, missing CONTROL_PLANE_PUBLIC_KEY does not fail boot and
   * unsigned HTTP is allowed. Dev/tests only (`VACPS_ALLOW_INSECURE_NO_AUTH=1`).
   */
  ALLOW_INSECURE_NO_AUTH: boolean;
  /** Extra filesystem roots allowed by path sandbox (absolute). */
  FS_ALLOWED_ROOTS: string[];
  REGISTRATION_TOKEN: string | undefined;
  REGISTRATION_INTERVAL_SECONDS: number;
  TELEMETRY_FALLBACK_INTERVAL_SECONDS: number;
}

function env(name: string): string | undefined {
  const v = host.getenv(name);
  if (v === null || v === '') return undefined;
  return v;
}

function envInt(name: string, fallback: number, min: number, max: number): number {
  const raw = env(name);
  if (raw === undefined) return fallback;
  const n = Number(raw);
  if (!Number.isInteger(n) || n < min || n > max) return fallback;
  return n;
}

function stripSlash(url: string | undefined): string | undefined {
  if (url === undefined) return undefined;
  return url.replace(/\/$/, '');
}

/** True when `url` is an absolute http(s) URL (required for control-plane baseUrl). */
export function isAbsoluteHttpUrl(url: string | undefined): url is string {
  if (!url || !url.trim()) return false;
  // Avoid new URL() if the QuickJS build lacks it; mirror common absolute-URL shape.
  return /^https?:\/\/[^\s/$.?#].[^\s]*$/i.test(url.trim());
}

function envTruthy(name: string): boolean {
  const v = env(name);
  if (v === undefined) return false;
  return v === '1' || v.toLowerCase() === 'true' || v.toLowerCase() === 'yes';
}

function parseAllowedRoots(): string[] {
  const raw = env('VACPS_FS_ALLOWED_ROOTS') ?? env('FS_ALLOWED_ROOTS') ?? '';
  return raw
    .split(/[:\n,]/)
    .map((s) => s.trim())
    .filter((s) => s.startsWith('/'));
}

export function loadConfig(): AgentConfig {
  const backendId = env('BACKEND_ID') ?? env('VACPS_BACKEND_ID') ?? 'local';
  const tagsRaw = env('BACKEND_TAGS') ?? '';
  const publicBase = stripSlash(env('PUBLIC_BASE_URL') ?? env('VACPS_PUBLIC_BASE_URL'));
  const controlPlane = stripSlash(env('CONTROL_PLANE_URL') ?? env('VACPS_CONTROL_PLANE_URL'));
  const listenHost = env('VACPS_LISTEN_HOST') ?? env('LISTEN_HOST') ?? '127.0.0.1';
  const listenPort = envInt('VACPS_LISTEN_PORT', envInt('LISTEN_PORT', 8788, 1, 65535), 1, 65535);
  return {
    BACKEND_ID: backendId,
    BACKEND_NAME: env('BACKEND_NAME') ?? backendId,
    BACKEND_TAGS: tagsRaw
      .split(',')
      .map((t) => t.trim())
      .filter(Boolean),
    LISTEN_HOST: listenHost,
    LISTEN_PORT: listenPort,
    // Drop non-absolute values so registration waits for quick-tunnel / managed URL.
    PUBLIC_BASE_URL: isAbsoluteHttpUrl(publicBase) ? publicBase : undefined,
    CONTROL_PLANE_URL: isAbsoluteHttpUrl(controlPlane) ? controlPlane : undefined,
    AGENT_PRIVATE_KEY: env('AGENT_PRIVATE_KEY') ?? env('VACPS_AGENT_PRIVATE_KEY'),
    AGENT_PUBLIC_KEY: env('AGENT_PUBLIC_KEY') ?? env('VACPS_AGENT_PUBLIC_KEY'),
    CONTROL_PLANE_PUBLIC_KEY:
      env('CONTROL_PLANE_PUBLIC_KEY') ?? env('VACPS_CONTROL_PLANE_PUBLIC_KEY'),
    ALLOW_INSECURE_NO_AUTH: envTruthy('VACPS_ALLOW_INSECURE_NO_AUTH'),
    FS_ALLOWED_ROOTS: parseAllowedRoots(),
    REGISTRATION_TOKEN: env('REGISTRATION_TOKEN') ?? env('VACPS_REGISTRATION_TOKEN'),
    REGISTRATION_INTERVAL_SECONDS: envInt('REGISTRATION_INTERVAL_SECONDS', 300, 60, 86_400),
    TELEMETRY_FALLBACK_INTERVAL_SECONDS: envInt(
      'TELEMETRY_FALLBACK_INTERVAL_SECONDS',
      120,
      15,
      3600,
    ),
  };
}

export function registrationConfigured(config: AgentConfig): boolean {
  return Boolean(
    config.CONTROL_PLANE_URL &&
    isAbsoluteHttpUrl(config.PUBLIC_BASE_URL) &&
    config.AGENT_PRIVATE_KEY &&
    config.AGENT_PUBLIC_KEY &&
    config.BACKEND_ID,
  );
}

export function telemetryConfigured(config: AgentConfig): boolean {
  return Boolean(config.CONTROL_PLANE_URL && config.AGENT_PRIVATE_KEY && config.BACKEND_ID);
}
