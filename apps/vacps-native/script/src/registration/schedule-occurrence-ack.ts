/**
 * POST /api/schedules/:id/occurrences/ack — agent-signed cursor ack to control plane.
 */
import type { ScheduleOccurrenceAck } from "@vacps/contracts";
import * as http from "vacps:http";
import * as log from "vacps:log";

import type { AgentConfig } from "../config";
import { telemetryConfigured } from "../config";
import { createAgentSignatureHeaders } from "../security/request-signatures";

function bodyText(body: ArrayBuffer): string {
  const u8 = new Uint8Array(body);
  try {
    const TD = (
      globalThis as {
        TextDecoder?: new (label?: string) => { decode(b: Uint8Array): string };
      }
    ).TextDecoder;
    if (TD) return new TD("utf-8").decode(u8);
  } catch {
    /* fall through */
  }
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]!);
  return s;
}

/**
 * Best-effort occurrence ack. Failures are logged; local cursor remains valid offline.
 */
export async function reportScheduleOccurrenceAck(
  config: AgentConfig,
  ack: Omit<ScheduleOccurrenceAck, "backend_id"> & { backend_id?: string },
): Promise<boolean> {
  if (!telemetryConfigured(config) || !config.AGENT_PRIVATE_KEY) return false;

  const payload: ScheduleOccurrenceAck = {
    backend_id: ack.backend_id ?? config.BACKEND_ID,
    schedule_id: ack.schedule_id,
    revision: ack.revision,
    scheduled_for: ack.scheduled_for,
    ...(ack.locally_advanced_to ? { locally_advanced_to: ack.locally_advanced_to } : {}),
    ...(ack.occurrence_id ? { occurrence_id: ack.occurrence_id } : {}),
    ...(ack.enqueued_count !== undefined ? { enqueued_count: ack.enqueued_count } : {}),
    ...(ack.claimed_at ? { claimed_at: ack.claimed_at } : {}),
  };

  const body = JSON.stringify(payload);
  const url = `${config.CONTROL_PLANE_URL}/api/schedules/${encodeURIComponent(payload.schedule_id)}/occurrences/ack`;
  const sig = createAgentSignatureHeaders(
    config.BACKEND_ID,
    config.AGENT_PRIVATE_KEY,
    "POST",
    url,
    body,
  );

  try {
    const res = await http.request({
      method: "POST",
      url,
      headers: {
        "content-type": "application/json",
        ...sig,
      },
      body,
      timeoutMs: 15_000,
    });
    if (res.status < 200 || res.status >= 300) {
      const text = bodyText(res.body);
      log.warn(
        `schedule ack failed id=${payload.schedule_id} status=${res.status} body=${text.slice(0, 160)}`,
      );
      return false;
    }
    log.info(
      `schedule ack ok id=${payload.schedule_id} rev=${payload.revision} scheduled_for=${payload.scheduled_for}`,
    );
    return true;
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    log.warn(`schedule ack error id=${payload.schedule_id}: ${msg}`);
    return false;
  }
}
