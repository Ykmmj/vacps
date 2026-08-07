import type { BackendHealth } from '@vacps/contracts';
import * as host from 'vacps:host';
import * as http from 'vacps:http';
import * as log from 'vacps:log';
import { Store } from 'vacps:store';
import { sleep } from 'vacps:timer';

import {
  loadConfig,
  registrationConfigured,
  telemetryConfigured,
  type AgentConfig,
} from './config';
import { ShellExecutor } from './executor/shell-executor';
import { TaskQueue } from './queue/task-queue';
import { SchedulerStore } from './queue/scheduler-store';
import {
  registerWithControlPlane,
  reportTelemetry,
} from './registration/control-plane-registration';
import { reportScheduleOccurrenceAck } from './registration/schedule-occurrence-ack';
import {
  loadControlPlaneState,
  saveControlPlaneState,
  type ControlPlaneState,
  type RegistrationStatus,
} from './registration/control-plane-state';
import { assertControlPlaneAuthConfig } from './security/http-auth';
import { createServer } from './server/app';
import { createInboundRequestAdapter } from './server/inbound-request';
import type { App } from './server/router';
import { ProcessSessions } from './runtime/process-sessions';
import { TaskStore } from './storage/task-store';
import {
  buildLivenessHealth,
  deriveLiveHealthState,
  type LiveHealthState,
} from './telemetry/liveness-health';
import { NativeTelemetryCollector } from './telemetry/native-telemetry';

/**
 * Composition root for the native agent script.
 * Host C++ calls only initialize / shutdown. Product scheduling lives here and
 * is driven by the generic Asio-backed vacps:timer capability.
 * Inbound HTTP: native transport event → JS Server onRequest callback (not a host export).
 *
 * Lifecycle contract:
 * - Configuration is immutable for one Application lifetime after initialize starts.
 * - `ready` is true only after registration/state work, listen, and both loop Promises.
 * - initialize owns staged rollback; background loop rejection triggers one product close.
 */
export class Application {
  /** Immutable after the start of initialize(). */
  private config!: AgentConfig;
  private db: Store | undefined;
  private store: TaskStore | undefined;
  private queue: TaskQueue | undefined;
  private telemetry: NativeTelemetryCollector | undefined;
  private processes: ProcessSessions | undefined;
  private httpApp: App | undefined;
  private server: http.Server | undefined;
  /** Per-application inbound adapter (owns request-id sequence). */
  private readonly adaptInboundRequest = createInboundRequestAdapter();
  private readonly startedMs = host.nowMs();
  private ready = false;
  private stopping = false;
  private controlLoop: Promise<void> | undefined;
  private workerLoop: Promise<void> | undefined;
  /** First background-loop failure preserved for shutdown to rethrow. */
  private loopFailure: Error | undefined;
  /** Single idempotent product-close path (shutdown + emergency). */
  private closePromise: Promise<void> | undefined;
  private cpState: ControlPlaneState = {
    registrationStatus: 'unknown',
    telemetryIntervalSeconds: 120,
  };
  private nextRegistrationMs = 0;
  private nextTelemetryMs = 0;

  async initialize(): Promise<void> {
    try {
      await this.initializeResources();
    } catch (error) {
      await this.rollbackPartialInit();
      throw error;
    }
  }

  private async initializeResources(): Promise<void> {
    this.config = loadConfig();
    assertControlPlaneAuthConfig(this.config);
    const path = `${host.dataDir()}/agent.db`;
    this.db = await Store.open(path);
    this.store = await TaskStore.create(this.db);
    const executor = new ShellExecutor(this.store);
    const schedulers = await SchedulerStore.create(this.db);
    this.queue = new TaskQueue(this.store, executor, schedulers);
    this.telemetry = new NativeTelemetryCollector(this.config, this.store);
    this.processes = new ProcessSessions(this.config.BACKEND_ID);

    const recovered = await this.queue.recoverInterruptedOnBoot();
    if (recovered > 0) {
      log.warn(`crash recovery: marked ${recovered} running task(s) agent_restarted`);
    }

    this.cpState = await loadControlPlaneState(this.db);
    if (!this.cpState.telemetryIntervalSeconds) {
      this.cpState.telemetryIntervalSeconds = this.config.TELEMETRY_FALLBACK_INTERVAL_SECONDS;
    }

    this.httpApp = await createServer({
      config: this.config,
      queue: this.queue,
      telemetry: this.telemetry,
      processes: this.processes,
      getControlPlaneState: () => this.cpState,
      isReady: () => this.ready,
      getLivenessHealth: () => this.getLivenessHealth(),
      getLiveHealthState: () => this.getLiveHealthState(),
    });

    // Bind address is agent policy (loadConfig / env). Transport stays route-free;
    // onRequest adapts the wire request into the business router contract.
    this.server = new http.Server(
      {
        host: this.config.LISTEN_HOST,
        port: this.config.LISTEN_PORT,
      },
      async (req) => {
        // HostResponse (string body) is a valid ServerResponse.
        return this.httpApp!.handleRequest(this.adaptInboundRequest(req));
      },
    );

    // Finish initial registration/state before listen + loops + ready.
    if (registrationConfigured(this.config)) {
      await this.runRegistration();
    } else {
      this.cpState = { ...this.cpState, registrationStatus: 'disabled' };
      await saveControlPlaneState(this.db, this.cpState);
    }
    this.nextTelemetryMs = host.nowMs() + 5_000;

    // Listen first. If listen fails, staged rollback closes the Server and no loops exist.
    const addr = await this.server.listen();
    // Install both loops then set ready with no await between — atomic at the JS-turn level.
    this.stopping = false;
    this.controlLoop = this.runControlLoop();
    this.workerLoop = this.runWorkerLoop();
    this.observeLoop('control', this.controlLoop);
    this.observeLoop('worker', this.workerLoop);
    this.ready = true;

    log.info(
      `application initialize host=${host.version()} platform=${host.platform()} db=${this.db.path} listening=${this.server.listening} addr=${addr.host}:${addr.port} controlPlane=${this.config.CONTROL_PLANE_URL ?? 'off'}`,
    );
  }

  private async runControlPlaneIteration(): Promise<void> {
    // Config is immutable for this Application lifetime — do not reload.
    const now = host.nowMs();
    if (registrationConfigured(this.config) && now >= this.nextRegistrationMs) {
      await this.runRegistration();
    }
    if (telemetryConfigured(this.config) && now >= this.nextTelemetryMs) {
      await this.runTelemetry();
    }
    // Absolute next_run_at claim → enqueue → concurrent best-effort CP occurrence acks
    const fired = await this.queue!.fireDueSchedulers();
    if (fired.acks.length > 0 && telemetryConfigured(this.config)) {
      await Promise.all(
        fired.acks.map((ack) =>
          reportScheduleOccurrenceAck(this.config, {
            schedule_id: ack.schedule_id,
            revision: ack.revision,
            scheduled_for: ack.scheduled_for,
            enqueued_count: ack.enqueued_count,
            claimed_at: this.isoNow(),
            ...(ack.locally_advanced_to ? { locally_advanced_to: ack.locally_advanced_to } : {}),
            ...(ack.occurrence_id ? { occurrence_id: ack.occurrence_id } : {}),
          }),
        ),
      );
    }
  }

  getControlPlaneState(): ControlPlaneState {
    return this.cpState;
  }

  /** Cheap public liveness snapshot (no df/uname/bash/telemetry collect). */
  getLivenessHealth(): BackendHealth {
    const live = this.getLiveHealthState();
    const version = host.version().slice(0, 48) || '0.1.0';
    return buildLivenessHealth({
      ...live,
      backendId: this.config.BACKEND_ID,
      version,
      uptimeSeconds: this.uptimeSeconds(),
    });
  }

  getLiveHealthState(): LiveHealthState {
    return deriveLiveHealthState({
      ready: this.ready,
      stopping: this.stopping,
      hasControlLoop: this.controlLoop !== undefined,
      hasWorkerLoop: this.workerLoop !== undefined,
      hasLoopFailure: this.loopFailure !== undefined,
      closing: this.closePromise !== undefined,
    });
  }

  async shutdown(): Promise<void> {
    let closeError: Error | undefined;
    try {
      await this.beginProductClose();
    } catch (error) {
      closeError = toError(error);
    }
    if (this.loopFailure !== undefined) {
      throw this.loopFailure;
    }
    if (closeError !== undefined) {
      throw closeError;
    }
  }

  private uptimeSeconds(): number {
    return Math.max(0, Math.floor((host.nowMs() - this.startedMs) / 1000));
  }

  private async runControlLoop(): Promise<void> {
    let lastRetentionMs = 0;
    let lastWorkerWakeMs = 0;
    while (!this.stopping) {
      await sleep(1_000);
      if (this.stopping) break;
      await this.runControlPlaneIteration();
      if (this.stopping) break;

      const now = host.nowMs();
      // Coarse reconciliation for missed/external DB work (not a second worker loop).
      if (now - lastWorkerWakeMs >= 5_000) {
        lastWorkerWakeMs = now;
        this.queue!.wakeWorker();
      }
      // Bounded SQLite retention; Store failure rejects this loop.
      if (now - lastRetentionMs >= 60_000) {
        lastRetentionMs = now;
        const pruned = await this.queue!.pruneRetention(now);
        if (pruned.outputsPruned > 0 || pruned.tasksDeleted > 0) {
          log.info(
            `retention prune outputs=${pruned.outputsPruned} tasks_deleted=${pruned.tasksDeleted}`,
          );
        }
      }
    }
  }

  private async runWorkerLoop(): Promise<void> {
    while (!this.stopping) {
      const worked = await this.queue!.pumpOnce();
      if (!worked && !this.stopping) {
        await this.queue!.waitForWork();
      }
    }
  }

  private observeLoop(name: string, loop: Promise<void>): void {
    void loop.then(
      () => {
        /* cooperative exit when stopping */
      },
      (error: unknown) => {
        this.onBackgroundLoopFailure(name, error);
      },
    );
  }

  private onBackgroundLoopFailure(name: string, error: unknown): void {
    if (this.loopFailure === undefined) {
      this.loopFailure = toError(error);
      log.error(`${name} loop failed: ${this.loopFailure.message}`);
      this.ready = false;
      this.stopping = true;
      // One idempotent emergency product close so ingress is not left listening.
      // Observe rejection so it is not unhandled; EntryModule shutdown awaits the same Promise.
      void this.beginProductClose().then(
        () => {
          /* close fulfilled; shutdown prefers saved loopFailure */
        },
        (closeError: unknown) => {
          log.error(`product close after ${name} loop failure: ${toError(closeError).message}`);
        },
      );
      // First failure only: EntryModule shutdown; saved loopFailure → nonzero exit.
      // A second requestStop is forced termination in C++ — never call it here again.
      host.requestStop();
    } else {
      log.error(`${name} loop failed (already recorded): ${toError(error).message}`);
    }
  }

  private beginProductClose(): Promise<void> {
    if (this.closePromise !== undefined) {
      return this.closePromise;
    }
    this.closePromise = this.runProductClose();
    return this.closePromise;
  }

  /**
   * Idempotent product close. Partial-state checks live only here (rollback boundary).
   * Order: mark not ready → stop ingress → process sessions → stop queue → join loops → Store.
   */
  private async runProductClose(): Promise<void> {
    log.info('application shutdown');
    this.ready = false;
    this.stopping = true;

    // Best-effort Server → Queue → join loops → Store; first resource close error is thrown after.
    let closeError: Error | undefined;

    if (this.server !== undefined) {
      try {
        await this.server.close();
      } catch (error) {
        const err = toError(error);
        if (closeError === undefined) {
          closeError = err;
          log.warn(`server close during product close: ${err.message}`);
        } else {
          log.warn(`server close during product close: ${err.message}`);
        }
      }
      this.server = undefined;
    }

    if (this.processes !== undefined) {
      try {
        await this.processes.close();
      } catch (error) {
        const err = toError(error);
        if (closeError === undefined) {
          closeError = err;
        }
        log.warn(`process sessions close during product close: ${err.message}`);
      }
      this.processes = undefined;
    }

    if (this.queue !== undefined) {
      try {
        await this.queue.stop();
      } catch (error) {
        const err = toError(error);
        if (closeError === undefined) {
          closeError = err;
        }
        log.warn(`queue stop during product close: ${err.message}`);
      }
    }

    await this.joinBackgroundLoops();

    if (this.db !== undefined) {
      try {
        await this.db.close();
      } catch (error) {
        const err = toError(error);
        if (closeError === undefined) {
          closeError = err;
        }
        log.warn(`store close during product close: ${err.message}`);
      }
      this.db = undefined;
    }

    this.queue = undefined;
    this.store = undefined;
    this.telemetry = undefined;
    this.httpApp = undefined;

    if (closeError !== undefined) {
      throw closeError;
    }
  }

  /** initialize() failure path — same close sequence, no second failure channel. */
  private async rollbackPartialInit(): Promise<void> {
    try {
      await this.beginProductClose();
    } catch (error) {
      log.warn(`rollback after initialize failure: ${toError(error).message}`);
    }
  }

  private async joinBackgroundLoops(): Promise<void> {
    const loops = [this.controlLoop, this.workerLoop];
    this.controlLoop = undefined;
    this.workerLoop = undefined;
    for (const loop of loops) {
      if (loop === undefined) continue;
      try {
        await loop;
      } catch (error) {
        if (this.loopFailure === undefined) {
          this.loopFailure = toError(error);
        }
      }
    }
  }

  private isoNow(): string {
    return new Date(host.nowMs()).toISOString();
  }

  private async runRegistration(): Promise<void> {
    try {
      const status = await registerWithControlPlane(this.config);
      const next: ControlPlaneState = {
        registrationStatus: toRegistrationStatus(status),
        lastRegistrationAt: this.isoNow(),
        telemetryIntervalSeconds: this.cpState.telemetryIntervalSeconds,
      };
      if (this.cpState.lastTelemetryAt) next.lastTelemetryAt = this.cpState.lastTelemetryAt;
      this.cpState = next;
      await saveControlPlaneState(this.db!, this.cpState);
      this.nextRegistrationMs = host.nowMs() + this.config.REGISTRATION_INTERVAL_SECONDS * 1000;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      log.warn(`registration failed: ${msg}`);
      this.cpState = { ...this.cpState, lastError: msg };
      await saveControlPlaneState(this.db!, this.cpState);
      this.nextRegistrationMs =
        host.nowMs() + Math.min(this.config.REGISTRATION_INTERVAL_SECONDS, 60) * 1000;
    }
  }

  private async runTelemetry(): Promise<void> {
    try {
      const status = await this.telemetry!.collect(this.getLiveHealthState());
      const interval = await reportTelemetry(this.config, status);
      const seconds =
        interval ??
        this.cpState.telemetryIntervalSeconds ??
        this.config.TELEMETRY_FALLBACK_INTERVAL_SECONDS;
      const next: ControlPlaneState = {
        registrationStatus: this.cpState.registrationStatus,
        lastTelemetryAt: this.isoNow(),
        telemetryIntervalSeconds: seconds,
      };
      if (this.cpState.lastRegistrationAt)
        next.lastRegistrationAt = this.cpState.lastRegistrationAt;
      this.cpState = next;
      await saveControlPlaneState(this.db!, this.cpState);
      this.nextTelemetryMs = host.nowMs() + seconds * 1000;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      log.warn(`telemetry failed: ${msg}`);
      this.cpState = { ...this.cpState, lastError: msg };
      await saveControlPlaneState(this.db!, this.cpState);
      this.nextTelemetryMs = host.nowMs() + this.config.TELEMETRY_FALLBACK_INTERVAL_SECONDS * 1000;
    }
  }
}

/** Normalize catch/rejection values once so even `undefined` is preserved as Error. */
function toError(error: unknown): Error {
  return error instanceof Error ? error : new Error(String(error));
}

function toRegistrationStatus(status: string | undefined): RegistrationStatus {
  if (
    status === 'pending' ||
    status === 'approved' ||
    status === 'rejected' ||
    status === 'disabled' ||
    status === 'unknown'
  ) {
    return status;
  }
  return 'unknown';
}
