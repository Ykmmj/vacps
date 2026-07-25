import { z } from 'zod';

const optionalUrl = z
  .preprocess((value) => (value === '' ? undefined : value), z.string().url().optional())
  .transform((value) => value?.replace(/\/$/, ''));

const configSchema = z.object({
  BACKEND_ID: z.string().regex(/^[a-z0-9-]{1,64}$/),
  BACKEND_NAME: z.string().trim().min(1).max(120).optional(),
  BACKEND_TAGS: z.string().default(''),
  BACKEND_SHARED_TOKEN: z.string().min(32),
  CONTROL_PLANE_URL: optionalUrl,
  PUBLIC_BASE_URL: optionalUrl,
  REGISTRATION_INTERVAL_SECONDS: z.coerce.number().int().min(60).max(86_400).default(300),
  LISTEN_HOST: z.string().default('127.0.0.1'),
  LISTEN_PORT: z.coerce.number().int().min(1).max(65_535).default(3100),
  REDIS_URL: z.string().url(),
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
