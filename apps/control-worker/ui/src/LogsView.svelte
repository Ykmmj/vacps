<script lang="ts">
  import { Button } from '$lib/components/ui/button/index.js';
  import { Skeleton } from '$lib/components/ui/skeleton/index.js';
  import RefreshCwIcon from '@lucide/svelte/icons/refresh-cw';
  import ScrollTextIcon from '@lucide/svelte/icons/scroll-text';
  import Trash2Icon from '@lucide/svelte/icons/trash-2';

  type TaskRecord = {
    id?: string;
    backendId?: string;
    kind?: string;
    status?: string;
    name?: string;
    summary?: string;
    environment?: string;
    retentionClass?: string;
    finishedAt?: string;
    terminalAt?: string;
    createdAt?: string;
  };

  type Props = {
    text: Record<string, string>;
    dashboard?: {
      failed?: TaskRecord[];
      totals?: Record<string, number>;
      retention?: {
        hide_test_by_default?: boolean;
        range_days?: number;
        test_deletable_count?: number;
        test_status_breakdown?: Record<string, number>;
      };
    };
    loading: boolean;
    cleaningTestHistory?: boolean;
    clearTestHistory?: () => void | Promise<void>;
    refresh: () => void | Promise<void>;
  };

  let {
    text,
    dashboard,
    loading,
    cleaningTestHistory = false,
    clearTestHistory,
    refresh,
  }: Props = $props();

  const failedTasks = $derived(dashboard?.failed ?? []);
  const testDeletable = $derived(
    dashboard?.retention?.test_deletable_count ?? dashboard?.totals?.testDeletable ?? 0,
  );
  const rangeDays = $derived(dashboard?.retention?.range_days ?? 7);
  const queueWaiting = $derived(dashboard?.totals?.queued ?? 0);
  const queueActive = $derived(dashboard?.totals?.active ?? 0);

  function label(key: string, fallback: string) {
    return text[key] ?? fallback;
  }

  function taskTitle(task: TaskRecord) {
    return task.name || task.summary || task.id || '—';
  }

  function relativeTime(value?: string) {
    if (!value) return '—';
    const milliseconds = Date.now() - new Date(value).getTime();
    if (!Number.isFinite(milliseconds) || milliseconds < 0) return '—';
    const seconds = Math.floor(milliseconds / 1000);
    if (seconds < 60) return `${seconds}s`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
    return `${Math.floor(seconds / 3600)}h`;
  }
</script>

<section class="logs-view" aria-label={label('logs', 'Logs')}>
  <div class="logs-toolbar">
    <div class="logs-title-block">
      <div class="logs-title-row">
        <span class="logs-icon" aria-hidden="true"><ScrollTextIcon /></span>
        <h1>{label('logs', 'Logs')}</h1>
      </div>
      <p>
        {label('logsHint', 'Last {days} days · test tasks hidden · soft-deleted hidden').replace(
          '{days}',
          String(rangeDays),
        )}
      </p>
    </div>
    <div class="logs-toolbar-actions">
      <span class="queue-pill">{label('queue', 'Queue')}: {queueWaiting}/{queueActive}</span>
      {#if testDeletable > 0 && clearTestHistory}
        <Button
          variant="outline"
          size="sm"
          class="cleanup-button"
          disabled={cleaningTestHistory || loading}
          onclick={() => clearTestHistory()}
        >
          <Trash2Icon />
          {cleaningTestHistory
            ? label('clearingTestHistory', 'Clearing…')
            : label('clearTestHistory', 'Clear test history').replace(
                '{count}',
                String(testDeletable),
              )}
        </Button>
      {/if}
      <Button
        variant="ghost"
        size="icon"
        class="refresh-button size-11 rounded-full"
        disabled={loading}
        onclick={() => refresh()}
        aria-label={label('refresh', 'Refresh')}
      >
        <RefreshCwIcon class={loading ? 'spin' : undefined} />
      </Button>
    </div>
  </div>

  <div class="logs-panel">
    {#if loading && !dashboard}
      <div class="logs-skeleton" aria-busy="true">
        {#each Array(5) as _}
          <Skeleton class="h-14 w-full rounded-xl" />
        {/each}
      </div>
    {:else if failedTasks.length === 0}
      <div class="logs-empty">
        <span class="logs-empty-icon" aria-hidden="true"><ScrollTextIcon /></span>
        <p>{label('noLogs', 'No recent non-test failures in this window.')}</p>
      </div>
    {:else}
      <ul class="task-list">
        {#each failedTasks as task (task.id)}
          <li class="task-row">
            <div class="task-main">
              <span class={`task-status ${task.status ?? 'failed'}`}>{task.status ?? 'failed'}</span
              >
              <span class="task-title" title={taskTitle(task)}>{taskTitle(task)}</span>
            </div>
            <div class="task-sub">
              <span class="task-backend-id">{task.backendId ?? '—'}</span>
              <span>{task.kind ?? '—'}</span>
              <span>{relativeTime(task.terminalAt ?? task.finishedAt ?? task.createdAt)}</span>
              {#if task.environment}
                <span class="task-env">{task.environment}</span>
              {/if}
            </div>
          </li>
        {/each}
      </ul>
    {/if}
  </div>
</section>

<style>
  .logs-view {
    animation: view-in 0.28s ease both;
  }
  .logs-toolbar {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    justify-content: space-between;
    gap: 1rem;
    margin-bottom: 1.25rem;
  }
  .logs-title-row {
    display: flex;
    align-items: center;
    gap: 0.55rem;
  }
  .logs-icon {
    display: grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: 0.65rem;
    background: color-mix(in oklch, var(--muted) 90%, transparent);
    color: var(--foreground);
  }
  .logs-icon :global(svg) {
    width: 1rem;
    height: 1rem;
  }
  .logs-title-block h1 {
    margin: 0;
    font-size: 1.25rem;
    font-weight: 650;
    letter-spacing: -0.02em;
  }
  .logs-title-block p {
    margin: 0.35rem 0 0;
    font-size: 0.8125rem;
    color: color-mix(in oklch, var(--muted-foreground) 94%, transparent);
  }
  .logs-toolbar-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .queue-pill {
    font-size: 0.75rem;
    padding: 0.25rem 0.55rem;
    border-radius: 999px;
    border: 1px solid color-mix(in oklch, var(--border) 80%, transparent);
    color: color-mix(in oklch, var(--muted-foreground) 95%, transparent);
  }
  .cleanup-button {
    gap: 0.35rem;
  }
  .logs-panel {
    padding: 1rem 1.1rem;
    border: 1px solid color-mix(in oklch, var(--border) 76%, transparent);
    border-radius: 1rem;
    background: color-mix(in oklch, var(--surface-soft) 92%, transparent);
    min-height: 12rem;
  }
  .logs-skeleton {
    display: grid;
    gap: 0.55rem;
  }
  .logs-empty {
    display: grid;
    place-items: center;
    gap: 0.65rem;
    padding: 2.5rem 1rem;
    text-align: center;
    color: color-mix(in oklch, var(--muted-foreground) 95%, transparent);
  }
  .logs-empty p {
    margin: 0;
    font-size: 0.9rem;
  }
  .logs-empty-icon {
    display: grid;
    place-items: center;
    width: 2.75rem;
    height: 2.75rem;
    border-radius: 0.9rem;
    background: color-mix(in oklch, var(--muted) 88%, transparent);
    color: color-mix(in oklch, var(--muted-foreground) 90%, transparent);
  }
  .logs-empty-icon :global(svg) {
    width: 1.15rem;
    height: 1.15rem;
  }
  .task-list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    gap: 0.45rem;
  }
  .task-row {
    display: grid;
    gap: 0.2rem;
    padding: 0.7rem 0.8rem;
    border-radius: 0.75rem;
    background: color-mix(in oklch, var(--background) 70%, transparent);
    border: 1px solid color-mix(in oklch, var(--border) 55%, transparent);
  }
  .task-main {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
  }
  .task-title {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 0.9rem;
  }
  .task-status {
    flex-shrink: 0;
    font-size: 0.68rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.02em;
    padding: 0.12rem 0.4rem;
    border-radius: 0.4rem;
    background: color-mix(in oklch, oklch(0.65 0.18 25) 18%, transparent);
    color: oklch(0.55 0.18 25);
  }
  .task-sub {
    display: flex;
    flex-wrap: wrap;
    gap: 0.55rem;
    font-size: 0.72rem;
    color: color-mix(in oklch, var(--muted-foreground) 95%, transparent);
  }
  .task-backend-id {
    font-family: var(--font-mono);
    overflow-wrap: anywhere;
  }
  .task-env {
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    overflow-wrap: anywhere;
  }
  :global(.spin) {
    animation: spin 0.8s linear infinite;
  }
  @keyframes view-in {
    from {
      opacity: 0;
      transform: translateY(5px);
    }
  }
  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (max-width: 640px) {
    .logs-toolbar {
      flex-direction: column;
      gap: 0.75rem;
      margin-bottom: 0.75rem;
    }
    .logs-toolbar-actions {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 2.75rem;
      width: 100%;
    }
    .queue-pill {
      grid-column: 1 / -1;
      width: fit-content;
      font-size: 0.75rem;
    }
    /* `:global` because the class lands on Button's root, which has no scope hash. */
    :global(.cleanup-button) {
      width: 100%;
      min-height: 2.75rem;
      white-space: normal;
    }
    .logs-panel {
      min-height: 0;
      padding: 0.75rem;
    }
    .logs-empty {
      padding: 1.5rem 0.75rem;
    }
    .task-row {
      gap: 0.4rem;
      padding: 0.75rem;
    }
    .task-main {
      align-items: flex-start;
    }
    .task-title {
      display: -webkit-box;
      overflow: hidden;
      line-clamp: 2;
      line-height: 1.4;
      overflow-wrap: anywhere;
      text-overflow: clip;
      white-space: normal;
      -webkit-box-orient: vertical;
      -webkit-line-clamp: 2;
    }
    .task-status {
      margin-top: 0.1rem;
      font-size: 0.75rem;
      line-height: 1.35;
    }
    .task-sub {
      gap: 0.35rem 0.65rem;
      font-size: 0.75rem;
      line-height: 1.4;
    }
    .task-backend-id {
      flex-basis: 100%;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .logs-view,
    :global(.spin) {
      animation: none;
    }
  }
</style>
