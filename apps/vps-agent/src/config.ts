import { z } from 'zod';

const optionalUrl = z
  .preprocess((value) => (value === '' ? undefined : value), z.url().optional())
  .transform((value) => value?.replace(/\/$/, ''));

const configSchema = z.object({
  BACKEND_ID: z.string().regex(/^[a-z0-9-]{1,64}$/),
  BACKEND_NAME: z.string().trim().min(1).max(120).optional(),
  BACKEND_TAGS: z.string().default(''),
  /** The installer generates this PKCS#8 Ed25519 key locally; it never leaves the VPS. */
  AGENT_PRIVATE_KEY: z.string().regex(/^[A-Za-z0-9_-]{64,256}$/),
  /** Raw Ed25519 public key paired with AGENT_PRIVATE_KEY. */
  AGENT_PUBLIC_KEY: z.string().regex(/^[A-Za-z0-9_-]{43}$/),
  /** Public half of the Worker control-plane signing key. */
  CONTROL_PLANE_PUBLIC_KEY: z.string().regex(/^[A-Za-z0-9_-]{43}$/),
  /** A short-lived, one-time registration capability. It is not used after enrollment. */
  REGISTRATION_TOKEN: z
    .string()
    .regex(/^[A-Za-z0-9_-]{43}$/)
    .optional(),
  CONTROL_PLANE_URL: optionalUrl,
  PUBLIC_BASE_URL: optionalUrl,
  REGISTRATION_INTERVAL_SECONDS: z.coerce.number().int().min(60).max(86_400).default(300),
  TELEMETRY_FALLBACK_INTERVAL_SECONDS: z.coerce.number().int().min(15).max(3600).default(120),
  LISTEN_HOST: z.string().default('127.0.0.1'),
  LISTEN_PORT: z.coerce.number().int().min(1).max(65_535).default(3100),
  REDIS_URL: z.url(),
  DATABASE_PATH: z.string().min(1).default('/var/lib/vps-agent/agent.db'),
  LOG_DIR: z.string().min(1).default('/var/lib/vps-agent/logs'),
  WORKER_CONCURRENCY: z.coerce.number().int().min(1).max(32).default(1),
  RUN_MODE: z.enum(['all', 'api', 'worker']).default('all'),
  PI_COMMAND: z.string().min(1).default('pi'),
  PI_COMMAND_ARGS_JSON: z.string().default('[]'),
  DEFAULT_PROFILE: z.literal('full').default('full'),
});

export type AgentConfig = Omit<z.infer<typeof configSchema>, 'PI_COMMAND_ARGS_JSON'> & {
  piCommandArgs: string[];
};

export function loadConfig(environment: NodeJS.ProcessEnv = process.env): AgentConfig {
  const parsed = configSchema.parse(environment);
  const { PI_COMMAND_ARGS_JSON: piCommandArgsJson, ...config } = parsed;
  const piCommandArgs = z.array(z.string()).parse(JSON.parse(piCommandArgsJson));
  return { ...config, piCommandArgs };
}
