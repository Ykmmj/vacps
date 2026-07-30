import { Application } from './application';
import type { HostRequest, HostResponse } from './contracts/http';
import type { TaskRequest, TaskResult } from './contracts/task';

/**
 * Host entry exports (C++ invoke_export).
 * Wiring mirrors apps/vacps/src/main.ts: config → store → executor → queue → server.
 */
let application: Application | undefined;

export async function initialize(): Promise<void> {
  if (application !== undefined) {
    throw new Error('VACPS script is already initialized');
  }
  const instance = new Application();
  await instance.initialize();
  application = instance;
}

export async function handleRequest(request: HostRequest): Promise<HostResponse> {
  if (application === undefined) {
    throw new Error('VACPS script is not initialized');
  }
  return application.handleRequest(request);
}

export async function runTask(task: TaskRequest): Promise<TaskResult> {
  if (application === undefined) {
    throw new Error('VACPS script is not initialized');
  }
  return application.runTask(task);
}

/** Invoked by C++ on a steady timer for registration/telemetry + task pump. */
export async function tickControlPlane(): Promise<void> {
  if (application === undefined) return;
  await application.tickControlPlane();
}

export async function shutdown(): Promise<void> {
  const instance = application;
  application = undefined;
  if (instance !== undefined) {
    await instance.shutdown();
  }
}
