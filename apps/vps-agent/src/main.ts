import { createServer } from './server/app.js';
import { loadConfig } from './config.js';
import { ShellExecutor } from './executor/shell-executor.js';
import { TaskGraphRunner } from './graph/task-graph.js';
import { PiRuntime } from './pi/pi-runtime.js';
import { FullAccessPolicy } from './policy/full-access-policy.js';
import { TaskQueue } from './queue/task-queue.js';
import { registerWithControlPlane } from './registration/control-plane-registration.js';
import { TaskStore } from './storage/task-store.js';

const config = loadConfig();
const store = new TaskStore(config.DATABASE_PATH);
const piRuntime = new PiRuntime(config.PI_COMMAND, config.piCommandArgs);
const runner = new TaskGraphRunner(
  store,
  new ShellExecutor(),
  new FullAccessPolicy(),
  piRuntime,
  config.LOG_DIR,
);
const queue = new TaskQueue(config, store, runner);

let registrationTimer: NodeJS.Timeout | undefined;

if (config.RUN_MODE !== 'api') await queue.startWorker();
if (config.RUN_MODE !== 'worker') {
  const server = await createServer({ config, queue, piRuntime });
  await server.listen({ host: config.LISTEN_HOST, port: config.LISTEN_PORT });
  if (config.CONTROL_PLANE_URL) {
    const register = async () => {
      try {
        const status = await registerWithControlPlane(config);
        console.info(`Control-plane registration status: ${status ?? 'not configured'}`);
      } catch (error) {
        console.warn(
          `Control-plane registration failed: ${error instanceof Error ? error.message : String(error)}`,
        );
      }
    };
    await register();
    registrationTimer = setInterval(
      () => void register(),
      config.REGISTRATION_INTERVAL_SECONDS * 1000,
    );
    registrationTimer.unref();
  }
}

async function shutdown(): Promise<void> {
  if (registrationTimer) clearInterval(registrationTimer);
  await queue.close();
  store.close();
  process.exit(0);
}

process.once('SIGINT', () => void shutdown());
process.once('SIGTERM', () => void shutdown());
