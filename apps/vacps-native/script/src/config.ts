import * as host from "vacps:host";

/**
 * Agent config from process env (parity with apps/vacps/src/config.ts).
 * Unset CONTROL_PLANE_URL disables registration/telemetry.
 */
export interface AgentConfig {
  BACKEND_ID: string;
  BACKEND_NAME: string;
  BACKEND_TAGS: string[];
  PUBLIC_BASE_URL: string | undefined;
  CONTROL_PLANE_URL: string | undefined;
  AGENT_PRIVATE_KEY: string | undefined;
  AGENT_PUBLIC_KEY: string | undefined;
  CONTROL_PLANE_PUBLIC_KEY: string | undefined;
  REGISTRATION_TOKEN: string | undefined;
  REGISTRATION_INTERVAL_SECONDS: number;
  TELEMETRY_FALLBACK_INTERVAL_SECONDS: number;
}

function env(name: string): string | undefined {
  const v = host.getenv(name);
  if (v === null || v === "") return undefined;
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
  return url.replace(/\/$/, "");
}

export function loadConfig(): AgentConfig {
  const backendId = env("BACKEND_ID") ?? env("VACPS_BACKEND_ID") ?? "local";
  const tagsRaw = env("BACKEND_TAGS") ?? "";
  return {
    BACKEND_ID: backendId,
    BACKEND_NAME: env("BACKEND_NAME") ?? backendId,
    BACKEND_TAGS: tagsRaw
      .split(",")
      .map((t) => t.trim())
      .filter(Boolean),
    PUBLIC_BASE_URL: stripSlash(env("PUBLIC_BASE_URL") ?? env("VACPS_PUBLIC_BASE_URL")),
    CONTROL_PLANE_URL: stripSlash(env("CONTROL_PLANE_URL") ?? env("VACPS_CONTROL_PLANE_URL")),
    AGENT_PRIVATE_KEY: env("AGENT_PRIVATE_KEY") ?? env("VACPS_AGENT_PRIVATE_KEY"),
    AGENT_PUBLIC_KEY: env("AGENT_PUBLIC_KEY") ?? env("VACPS_AGENT_PUBLIC_KEY"),
    CONTROL_PLANE_PUBLIC_KEY:
      env("CONTROL_PLANE_PUBLIC_KEY") ?? env("VACPS_CONTROL_PLANE_PUBLIC_KEY"),
    REGISTRATION_TOKEN: env("REGISTRATION_TOKEN") ?? env("VACPS_REGISTRATION_TOKEN"),
    REGISTRATION_INTERVAL_SECONDS: envInt("REGISTRATION_INTERVAL_SECONDS", 300, 60, 86_400),
    TELEMETRY_FALLBACK_INTERVAL_SECONDS: envInt(
      "TELEMETRY_FALLBACK_INTERVAL_SECONDS",
      120,
      15,
      3600,
    ),
  };
}

export function registrationConfigured(config: AgentConfig): boolean {
  return Boolean(
    config.CONTROL_PLANE_URL &&
      config.PUBLIC_BASE_URL &&
      config.AGENT_PRIVATE_KEY &&
      config.AGENT_PUBLIC_KEY &&
      config.BACKEND_ID,
  );
}

export function telemetryConfigured(config: AgentConfig): boolean {
  return Boolean(config.CONTROL_PLANE_URL && config.AGENT_PRIVATE_KEY && config.BACKEND_ID);
}
