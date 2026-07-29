/**
 * Minimal in-memory D1 for TaskService hard-cleanup acceptance tests.
 * Supports the SQL shapes TaskService issues (not a general SQL engine).
 */

export type FakeRow = Record<string, unknown>;

function norm(sql: string): string {
  return sql.replace(/\s+/g, ' ').trim();
}

export class FakeD1 {
  tasks: FakeRow[] = [];
  createIdem: FakeRow[] = [];
  opIdem: FakeRow[] = [];

  prepare(sql: string) {
    const statement = norm(sql);
    const exec = (binds: unknown[]) => this.execute(statement, binds);
    return {
      bind: (...binds: unknown[]) => ({
        run: async () => exec(binds),
        first: async <T>() => (exec(binds).first as T | null) ?? null,
        all: async <T>() => ({ results: exec(binds).all as T[] }),
      }),
      run: async () => exec([]),
      first: async <T>() => (exec([]).first as T | null) ?? null,
      all: async <T>() => ({ results: exec([]).all as T[] }),
    };
  }

  private execute(sql: string, binds: unknown[]): { first: FakeRow | null; all: FakeRow[] } {
    // ── tasks INSERT ──────────────────────────────────────────────
    if (sql.startsWith('INSERT INTO tasks') || sql.startsWith('INSERT OR IGNORE INTO tasks')) {
      const cols = sql
        .slice(sql.indexOf('(') + 1, sql.indexOf(')'))
        .split(',')
        .map((c) => c.trim());
      const row: FakeRow = {
        terminal_at: null,
        expires_at: null,
        labels_json: null,
        environment: null,
        retention_class: null,
        deleted_at: null,
        deleted_by: null,
        deletion_reason: null,
        cleanup_state: 'none',
        finished_at: null,
        schedule_id: null,
        idempotency_key: null,
        request_hash: null,
        retry_of_task_id: null,
        name: null,
        summary: null,
        kind: null,
      };
      let bi = 0;
      for (const col of cols) {
        if (sql.includes(`'created'`) && col === 'status' && !sql.includes('VALUES')) {
          // values use literal 'created' for status sometimes
        }
        // VALUES clause: count ? placeholders
      }
      // Parse VALUES bind order from known create/retry shapes.
      if (sql.includes("VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'created'")) {
        // create path
        const [
          id,
          backend_id,
          type,
          kind,
          source,
          profile,
          name,
          summary,
          schedule_id,
          idempotency_key,
          request_hash,
          created_at,
          updated_at,
          labels_json,
          environment,
          retention_class,
        ] = binds;
        Object.assign(row, {
          id,
          backend_id,
          type,
          kind,
          source,
          profile,
          name,
          summary,
          status: 'created',
          schedule_id,
          idempotency_key,
          request_hash,
          created_at,
          updated_at,
          labels_json,
          environment,
          retention_class,
          cleanup_state: 'none',
        });
      } else if (sql.includes("VALUES (?, ?, ?, ?, 'mcp'")) {
        // retry path
        const [
          id,
          backend_id,
          type,
          kind,
          profile,
          name,
          summary,
          schedule_id,
          retry_of,
          created_at,
          updated_at,
          labels_json,
          environment,
          retention_class,
        ] = binds;
        Object.assign(row, {
          id,
          backend_id,
          type,
          kind,
          source: 'mcp',
          profile,
          name,
          summary,
          status: 'queued',
          schedule_id,
          retry_of_task_id: retry_of,
          created_at,
          updated_at,
          labels_json,
          environment,
          retention_class,
          cleanup_state: 'none',
        });
      } else {
        // generic: map remaining ? to columns in order
        let i = 0;
        for (const col of cols) {
          if (i < binds.length) row[col] = binds[i++];
        }
      }
      const ignore = sql.includes('OR IGNORE') && this.tasks.some((t) => t.id === row.id);
      if (!ignore) this.tasks.push(row);
      return { first: null, all: [] };
    }

    // ── task_create_idempotency UPSERT ────────────────────────────
    if (sql.startsWith('INSERT INTO task_create_idempotency')) {
      const [
        backend_id,
        idempotency_key,
        request_hash,
        task_id,
        task_deleted,
        original_status,
        original_created_at,
        created_at,
        expires_at,
      ] = binds;
      const idx = this.createIdem.findIndex(
        (r) => r.backend_id === backend_id && r.idempotency_key === idempotency_key,
      );
      const next = {
        backend_id,
        idempotency_key,
        request_hash,
        task_id,
        task_deleted,
        original_status,
        original_created_at,
        created_at,
        expires_at,
      };
      if (idx >= 0) {
        const prev = this.createIdem[idx]!;
        this.createIdem[idx] = {
          ...next,
          original_created_at: prev.original_created_at ?? original_created_at,
        };
      } else {
        this.createIdem.push(next);
      }
      return { first: null, all: [] };
    }

    // ── operation_idempotency ─────────────────────────────────────
    if (sql.startsWith('INSERT OR REPLACE INTO operation_idempotency')) {
      const [scope, idempotency_key, request_hash, result_json, created_at] = binds;
      const idx = this.opIdem.findIndex(
        (r) => r.scope === scope && r.idempotency_key === idempotency_key,
      );
      const row = { scope, idempotency_key, request_hash, result_json, created_at };
      if (idx >= 0) this.opIdem[idx] = row;
      else this.opIdem.push(row);
      return { first: null, all: [] };
    }

    if (sql.startsWith('SELECT request_hash, result_json FROM operation_idempotency')) {
      const [scope, key] = binds;
      const row = this.opIdem.find((r) => r.scope === scope && r.idempotency_key === key) ?? null;
      return { first: row, all: row ? [row] : [] };
    }

    // ── task_create_idempotency SELECT ────────────────────────────
    if (sql.includes('FROM task_create_idempotency') && sql.includes('SELECT')) {
      if (sql.includes('DELETE FROM task_create_idempotency WHERE expires_at')) {
        // handled below
      } else if (sql.includes('expires_at >')) {
        const [backend_id, idempotency_key, now] = binds as [string, string, string];
        const row =
          this.createIdem.find(
            (r) =>
              r.backend_id === backend_id &&
              r.idempotency_key === idempotency_key &&
              String(r.expires_at) > now,
          ) ?? null;
        return { first: row, all: row ? [row] : [] };
      } else if (sql.includes('backend_id = ? AND idempotency_key = ?') && sql.includes('DELETE')) {
        // fall through
      }
    }

    if (sql.startsWith('DELETE FROM task_create_idempotency WHERE backend_id')) {
      const [backend_id, key] = binds;
      this.createIdem = this.createIdem.filter(
        (r) => !(r.backend_id === backend_id && r.idempotency_key === key),
      );
      return { first: null, all: [] };
    }

    if (sql.startsWith('DELETE FROM task_create_idempotency WHERE expires_at')) {
      const [now] = binds as [string];
      this.createIdem = this.createIdem.filter((r) => String(r.expires_at) > now);
      return { first: null, all: [] };
    }

    // ── tasks DELETE ──────────────────────────────────────────────
    if (sql === 'DELETE FROM tasks WHERE id = ?') {
      const [id] = binds;
      this.tasks = this.tasks.filter((t) => t.id !== id);
      return { first: null, all: [] };
    }

    // ── tasks UPDATE status (non-terminal) ────────────────────────
    if (sql === 'UPDATE tasks SET status = ?, updated_at = ? WHERE id = ?') {
      const [status, updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.status = status;
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }

    // ── tasks UPDATE terminal setStatus ───────────────────────────
    if (sql.includes('UPDATE tasks SET') && sql.includes('terminal_at = COALESCE')) {
      const [
        status,
        updated_at,
        finished,
        terminalAt,
        expiresAt,
        outputExpiresAt,
        retentionClass,
        id,
      ] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.status = status;
        row.updated_at = updated_at;
        row.finished_at = row.finished_at ?? finished;
        row.terminal_at = row.terminal_at ?? terminalAt;
        row.expires_at = row.expires_at ?? expiresAt;
        row.output_expires_at = row.output_expires_at ?? outputExpiresAt;
        if (!row.retention_class) row.retention_class = retentionClass;
        if (!row.cleanup_state || row.cleanup_state === 'none') row.cleanup_state = 'eligible';
      }
      return { first: null, all: [] };
    }

    // ── pin / legal hold UPDATE ───────────────────────────────────
    if (sql.includes('SET pinned_at = ?') && sql.includes('pinned_by = ?')) {
      const [pinned_at, pinned_by, updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.pinned_at = pinned_at;
        row.pinned_by = pinned_by;
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }
    if (sql.includes('SET pinned_at = NULL') && sql.includes('pinned_by = NULL')) {
      const [expires_at, updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.pinned_at = null;
        row.pinned_by = null;
        if (expires_at != null) row.expires_at = expires_at;
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }
    if (sql.includes('SET legal_hold = 1')) {
      const [reason, at, by, updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.legal_hold = 1;
        row.legal_hold_reason = reason;
        row.legal_hold_at = at;
        row.legal_hold_by = by;
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }
    if (sql.includes('SET legal_hold = 0')) {
      const [updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row) {
        row.legal_hold = 0;
        row.legal_hold_reason = null;
        row.legal_hold_at = null;
        row.legal_hold_by = null;
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }

    // ── soft delete UPDATE ────────────────────────────────────────
    if (sql.includes('SET deleted_at = ?') && sql.includes("cleanup_state = 'deleted'")) {
      const [deleted_at, deleted_by, deletion_reason, updated_at, id] = binds;
      const row = this.tasks.find((t) => t.id === id);
      if (row && !row.deleted_at) {
        row.deleted_at = deleted_at;
        row.deleted_by = deleted_by;
        row.deletion_reason = deletion_reason;
        row.cleanup_state = 'deleted';
        row.updated_at = updated_at;
      }
      return { first: null, all: [] };
    }

    // ── SELECT labels for setStatus ───────────────────────────────
    if (sql.includes('SELECT labels_json, environment, terminal_at, retention_class FROM tasks')) {
      const [id] = binds;
      const row = this.tasks.find((t) => t.id === id) ?? null;
      return {
        first: row
          ? {
              labels_json: row.labels_json,
              environment: row.environment,
              terminal_at: row.terminal_at,
              retention_class: row.retention_class,
            }
          : null,
        all: [],
      };
    }

    // ── SELECT * FROM tasks WHERE id ──────────────────────────────
    if (sql === 'SELECT * FROM tasks WHERE id = ?') {
      const [id] = binds;
      const row = this.tasks.find((t) => t.id === id) ?? null;
      return { first: row, all: row ? [row] : [] };
    }

    // ── SELECT by idempotency on tasks ────────────────────────────
    if (sql.includes('FROM tasks WHERE backend_id = ? AND idempotency_key = ?')) {
      const [backend_id, key] = binds;
      const row =
        this.tasks.find((t) => t.backend_id === backend_id && t.idempotency_key === key) ?? null;
      return { first: row, all: row ? [row] : [] };
    }

    // ── list / cleanup SELECT * / SELECT id,status ────────────────
    if (sql.startsWith('SELECT') && sql.includes('FROM tasks')) {
      let rows = [...this.tasks];
      // Apply filters present in SQL + binds in order of appearance.
      // We parse simple `col = ?` and known clauses.
      const clauses: Array<{ kind: string; value?: unknown }> = [];
      const parts = sql.split('WHERE')[1]?.split('ORDER BY')[0] ?? '';
      // Extract bind placeholders in order.
      const tokenRe =
        /(deleted_at IS NULL)|(?:backend_id = \?)|(?:status = \?)|(?:source = \?)|(?:environment = \?)|(?:schedule_id = \?)|(?:\(kind = \? OR type = \?\))|(?:created_at >= \?)|(?:created_at <= \?)|(?:terminal_at IS NOT NULL AND terminal_at <= \?)|(?:expires_at IS NOT NULL AND expires_at <= \?)|(?:status IN \('succeeded','failed','cancelled','timed_out','dispatch_failed'\))|(?:status NOT IN \('succeeded','failed','cancelled','timed_out','dispatch_failed'\))|(?:\(environment IS NULL OR environment != 'test'\) AND \(retention_class IS NULL OR retention_class != 'test'\))|(?:\(environment = 'test' OR retention_class = 'test'\))|(?:COALESCE\(legal_hold, 0\) = 0)|(?:pinned_at IS NULL)|(?:deleted_at IS NOT NULL AND deleted_at <= \?)|(?:json_extract\(labels_json, \?\) = \?)/g;
      let m: RegExpExecArray | null;
      let bi = 0;
      while ((m = tokenRe.exec(parts))) {
        const tok = m[0];
        if (tok === 'deleted_at IS NULL') {
          rows = rows.filter((r) => !r.deleted_at);
        } else if (tok === 'backend_id = ?') {
          const v = binds[bi++];
          rows = rows.filter((r) => r.backend_id === v);
        } else if (tok === 'status = ?') {
          const v = binds[bi++];
          rows = rows.filter((r) => r.status === v);
        } else if (tok === 'source = ?') {
          const v = binds[bi++];
          rows = rows.filter((r) => r.source === v);
        } else if (tok === 'environment = ?') {
          const v = binds[bi++];
          rows = rows.filter((r) => r.environment === v);
        } else if (tok === 'schedule_id = ?') {
          const v = binds[bi++];
          rows = rows.filter((r) => r.schedule_id === v);
        } else if (tok.includes('kind = ? OR type')) {
          const v = binds[bi++];
          binds[bi++]; // duplicate kind/type bind
          rows = rows.filter((r) => r.kind === v || r.type === v);
        } else if (tok === 'created_at >= ?') {
          const v = String(binds[bi++]);
          rows = rows.filter((r) => String(r.created_at) >= v);
        } else if (tok === 'created_at <= ?') {
          const v = String(binds[bi++]);
          rows = rows.filter((r) => String(r.created_at) <= v);
        } else if (tok.includes('terminal_at <= ?')) {
          const v = String(binds[bi++]);
          rows = rows.filter((r) => r.terminal_at && String(r.terminal_at) <= v);
        } else if (tok.includes('expires_at <= ?')) {
          const v = String(binds[bi++]);
          rows = rows.filter((r) => r.expires_at && String(r.expires_at) <= v);
        } else if (tok.includes("status IN ('succeeded'")) {
          rows = rows.filter((r) =>
            ['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
              String(r.status),
            ),
          );
        } else if (tok.includes('status NOT IN')) {
          rows = rows.filter(
            (r) =>
              !['succeeded', 'failed', 'cancelled', 'timed_out', 'dispatch_failed'].includes(
                String(r.status),
              ),
          );
        } else if (tok.includes("environment != 'test'")) {
          rows = rows.filter((r) => r.environment !== 'test' && r.retention_class !== 'test');
        } else if (tok.includes("environment = 'test' OR retention_class = 'test'")) {
          rows = rows.filter((r) => r.environment === 'test' || r.retention_class === 'test');
        } else if (tok.includes('COALESCE(legal_hold, 0) = 0')) {
          rows = rows.filter((r) => !r.legal_hold);
        } else if (tok === 'pinned_at IS NULL') {
          rows = rows.filter((r) => !r.pinned_at);
        } else if (tok.includes('deleted_at <= ?')) {
          const v = String(binds[bi++]);
          rows = rows.filter((r) => r.deleted_at && String(r.deleted_at) <= v);
        } else if (tok.startsWith('json_extract')) {
          const path = String(binds[bi++]); // $.key
          const val = binds[bi++];
          const key = path.replace('$.', '');
          rows = rows.filter((r) => {
            try {
              const labels = r.labels_json ? JSON.parse(String(r.labels_json)) : {};
              return labels[key] === val;
            } catch {
              return false;
            }
          });
        }
      }

      // ORDER BY
      if (sql.includes('ORDER BY created_at DESC')) {
        rows.sort((a, b) => String(b.created_at).localeCompare(String(a.created_at)));
      } else if (sql.includes('ORDER BY created_at ASC')) {
        rows.sort((a, b) => String(a.created_at).localeCompare(String(b.created_at)));
      } else if (sql.includes('ORDER BY expires_at ASC')) {
        rows.sort((a, b) => String(a.expires_at ?? '').localeCompare(String(b.expires_at ?? '')));
      } else if (sql.includes('ORDER BY deleted_at ASC')) {
        rows.sort((a, b) => String(a.deleted_at ?? '').localeCompare(String(b.deleted_at ?? '')));
      }

      // LIMIT ? OFFSET ?
      if (sql.includes('LIMIT ? OFFSET ?')) {
        const limit = Number(binds[binds.length - 2]);
        const offset = Number(binds[binds.length - 1]);
        rows = rows.slice(offset, offset + limit);
      } else if (sql.includes('LIMIT ?')) {
        const limit = Number(binds[binds.length - 1]);
        rows = rows.slice(0, limit);
      }

      if (sql.startsWith('SELECT id, status FROM tasks')) {
        rows = rows.map((r) => ({ id: r.id, status: r.status }));
      } else if (sql.startsWith('SELECT id FROM tasks')) {
        rows = rows.map((r) => ({ id: r.id }));
      }

      return { first: rows[0] ?? null, all: rows };
    }

    throw new Error(`FakeD1 unsupported SQL: ${sql}\n binds=${JSON.stringify(binds)}`);
  }
}

export function asD1(db: FakeD1): D1Database {
  return db as unknown as D1Database;
}
