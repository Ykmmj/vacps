import { taskDispatchSchema } from "@vacps/contracts";
import * as host from "vacps:host";
import * as http from "vacps:http";
import * as log from "vacps:log";
import * as store from "vacps:store";

import {
  loadConfig,
  registrationConfigured,
  telemetryConfigured,
  type AgentConfig,
} from "./config";
import type { HostRequest, HostResponse } from "./contracts/http";
import type { TaskRequest, TaskResult } from "./contracts/task";
import { TaskState } from "./contracts/task";
import { ShellExecutor } from "./executor/shell-executor";
import { TaskQueue } from "./queue/task-queue";
import { SchedulerStore } from "./queue/scheduler-store";
import {
  registerWithControlPlane,
  reportTelemetry,
} from "./registration/control-plane-registration";
import { reportScheduleOccurrenceAck } from "./registration/schedule-occurrence-ack";
import {
  loadControlPlaneState,
  saveControlPlaneState,
  type ControlPlaneState,
  type RegistrationStatus,
} from "./registration/control-plane-state";
import { ProcessManager } from "./runtime/process-manager";
import { createServer } from "./server/app";
import type { App } from "./server/router";
import { TaskStore } from "./storage/task-store";
import { NativeTelemetryCollector } from "./telemetry/native-telemetry";

type Store = ReturnType<typeof store.open>;
type HttpServer = ReturnType<typeof http.createServer>;

/**
 * Composition root for native agent script (apps/vacps main wiring, class form).
 * Host C++ calls initialize / handleRequest / tickControlPlane / shutdown.
 */
export class Application {
  private config: AgentConfig = loadConfig();
  private db: Store | undefined;
  private store: TaskStore | undefined;
  private queue: TaskQueue | undefined;
  private processes: ProcessManager | undefined;
  private telemetry: NativeTelemetryCollector | undefined;
  private httpApp: App | undefined;
  private server: HttpServer | undefined;
  private ready = false;
  private cpState: ControlPlaneState = {
    registrationStatus: "unknown",
    telemetryIntervalSeconds: 120,
  };
  private nextRegistrationMs = 0;
  private nextTelemetryMs = 0;

  async initialize(): Promise<void> {
    this.config = loadConfig();
    const path = `${host.dataDir()}/agent.db`;
    this.db = store.open(path);
    this.store = new TaskStore(this.db);
    const executor = new ShellExecutor(this.store);
    const schedulers = new SchedulerStore(this.db);
    this.queue = new TaskQueue(this.store, executor, schedulers);
    this.processes = new ProcessManager(this.config.BACKEND_ID);
    this.telemetry = new NativeTelemetryCollector(this.config, this.store);

    const recovered = this.queue.recoverInterruptedOnBoot();
    if (recovered > 0) {
      log.warn(`crash recovery: marked ${recovered} running task(s) agent_restarted`);
    }

    this.cpState = loadControlPlaneState(this.db);
    if (!this.cpState.telemetryIntervalSeconds) {
      this.cpState.telemetryIntervalSeconds = this.config.TELEMETRY_FALLBACK_INTERVAL_SECONDS;
    }

    this.httpApp = await createServer({
      config: this.config,
      queue: this.queue,
      processes: this.processes,
      telemetry: this.telemetry,
      getControlPlaneState: () => this.cpState,
      isReady: () => this.ready,
    });

    this.server = http.createServer();
    this.server.listen();
    this.ready = true;

    log.info(
      `application initialize host=${host.version()} platform=${host.platform()} db=${this.db.path()} listening=${this.server.isListening()} controlPlane=${this.config.CONTROL_PLANE_URL ?? "off"}`,
    );

    if (registrationConfigured(this.config)) {
      await this.runRegistration();
    } else {
      this.cpState = { ...this.cpState, registrationStatus: "disabled" };
      saveControlPlaneState(this.db, this.cpState);
    }
    this.nextTelemetryMs = host.nowMs() + 5_000;
  }

  async handleRequest(request: HostRequest): Promise<HostResponse> {
    if (!this.httpApp) {
      return {
        status: 503,
        headers: { "content-type": "application/json; charset=utf-8" },
        body: JSON.stringify({
          error: { code: "service_unavailable", message: "application not initialized" },
        }),
      };
    }
    return this.httpApp.handleRequest(request);
  }

  async runTask(task: TaskRequest): Promise<TaskResult> {
    if (!this.ready || !this.queue || !this.store) {
      throw new Error("application not initialized");
    }
    try {
      const parsed = taskDispatchSchema.safeParse(JSON.parse(task.payload || "{}"));
      if (parsed.success) {
        this.queue.enqueue(parsed.data);
        await this.queue.pumpOnce();
        const done = this.queue.getTask(parsed.data.task_id);
        return {
          id: task.id,
          state:
            done?.status === "succeeded"
              ? TaskState.succeeded
              : done?.status === "cancelled"
                ? TaskState.cancelled
                : TaskState.failed,
          message: done?.error?.message ?? done?.status ?? "done",
        };
      }
    } catch {
      /* fall through */
    }
    return {
      id: task.id,
      state: TaskState.failed,
      message: "invalid task payload; expected TaskDispatch JSON",
    };
  }

  async tickControlPlane(): Promise<void> {
    if (!this.ready || !this.db || !this.queue) return;
    this.config = loadConfig();
    const now = host.nowMs();
    if (registrationConfigured(this.config) && now >= this.nextRegistrationMs) {
      await this.runRegistration();
    }
    if (telemetryConfigured(this.config) && now >= this.nextTelemetryMs) {
      await this.runTelemetry();
    }
    // Absolute next_run_at claim → enqueue → best-effort CP occurrence ack → task pump
    const fired = this.queue.fireDueSchedulers();
    if (fired.acks.length > 0 && telemetryConfigured(this.config)) {
      for (const ack of fired.acks) {
        await reportScheduleOccurrenceAck(this.config, {
          schedule_id: ack.schedule_id,
          revision: ack.revision,
          scheduled_for: ack.scheduled_for,
          enqueued_count: ack.enqueued_count,
          claimed_at: this.isoNow(),
          ...(ack.locally_advanced_to
            ? { locally_advanced_to: ack.locally_advanced_to }
            : {}),
          ...(ack.occurrence_id ? { occurrence_id: ack.occurrence_id } : {}),
        });
      }
    }
    await this.queue.pumpOnce();
  }

  getControlPlaneState(): ControlPlaneState {
    return this.cpState;
  }

  async shutdown(): Promise<void> {
    log.info("application shutdown");
    if (this.server) {
      this.server.close();
      this.server = undefined;
    }
    if (this.db) {
      this.db.close();
      this.db = undefined;
    }
    this.store = undefined;
    this.queue = undefined;
    this.processes = undefined;
    this.telemetry = undefined;
    this.httpApp = undefined;
    this.ready = false;
  }

  private isoNow(): string {
    return new Date(host.nowMs()).toISOString();
  }

  private async runRegistration(): Promise<void> {
    if (!this.db || !registrationConfigured(this.config)) return;
    try {
      const status = await registerWithControlPlane(this.config);
      const next: ControlPlaneState = {
        registrationStatus: toRegistrationStatus(status),
        lastRegistrationAt: this.isoNow(),
        telemetryIntervalSeconds: this.cpState.telemetryIntervalSeconds,
      };
      if (this.cpState.lastTelemetryAt) next.lastTelemetryAt = this.cpState.lastTelemetryAt;
      this.cpState = next;
      saveControlPlaneState(this.db, this.cpState);
      this.nextRegistrationMs =
        host.nowMs() + this.config.REGISTRATION_INTERVAL_SECONDS * 1000;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      log.warn(`registration failed: ${msg}`);
      this.cpState = { ...this.cpState, lastError: msg };
      saveControlPlaneState(this.db, this.cpState);
      this.nextRegistrationMs =
        host.nowMs() + Math.min(this.config.REGISTRATION_INTERVAL_SECONDS, 60) * 1000;
    }
  }

  private async runTelemetry(): Promise<void> {
    if (!this.db || !telemetryConfigured(this.config) || !this.telemetry) return;
    try {
      const status = await this.telemetry.collect();
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
      if (this.cpState.lastRegistrationAt) next.lastRegistrationAt = this.cpState.lastRegistrationAt;
      this.cpState = next;
      saveControlPlaneState(this.db, this.cpState);
      this.nextTelemetryMs = host.nowMs() + seconds * 1000;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      log.warn(`telemetry failed: ${msg}`);
      this.cpState = { ...this.cpState, lastError: msg };
      saveControlPlaneState(this.db, this.cpState);
      this.nextTelemetryMs =
        host.nowMs() + this.config.TELEMETRY_FALLBACK_INTERVAL_SECONDS * 1000;
    }
  }
}

function toRegistrationStatus(status: string | undefined): RegistrationStatus {
  if (
    status === "pending" ||
    status === "approved" ||
    status === "rejected" ||
    status === "disabled" ||
    status === "unknown"
  ) {
    return status;
  }
  return "unknown";
}
