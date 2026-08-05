import { Application } from './application';

/**
 * Host entry exports (C++ invoke_export).
 * Lifecycle: initialize / shutdown.
 * Product control and task loops are owned by Application and use vacps:timer.
 * Inbound HTTP is native event → JS Server onRequest callback (not a host export).
 */
let application: Application | undefined;

export async function initialize(): Promise<void> {
  if (application !== undefined) {
    throw new Error('VACPS script is already initialized');
  }
  // Retain the instance before await so a failed initialize can roll back via Application.
  const instance = new Application();
  application = instance;
  try {
    await instance.initialize();
  } catch (error) {
    application = undefined;
    throw error;
  }
}

export async function shutdown(): Promise<void> {
  const instance = application;
  application = undefined;
  if (instance !== undefined) {
    await instance.shutdown();
  }
}
