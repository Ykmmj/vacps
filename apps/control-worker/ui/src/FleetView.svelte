<script lang="ts">
  import { Badge } from '$lib/components/ui/badge/index.js';
  import { Button } from '$lib/components/ui/button/index.js';
  import * as Card from '$lib/components/ui/card/index.js';
  import * as DropdownMenu from '$lib/components/ui/dropdown-menu/index.js';
  import * as Sheet from '$lib/components/ui/sheet/index.js';
  import { Skeleton } from '$lib/components/ui/skeleton/index.js';
  import ActivityIcon from '@lucide/svelte/icons/activity';
  import DownloadIcon from '@lucide/svelte/icons/download';
  import CheckIcon from '@lucide/svelte/icons/check';
  import CopyIcon from '@lucide/svelte/icons/copy';
  import CpuIcon from '@lucide/svelte/icons/cpu';
  import EllipsisIcon from '@lucide/svelte/icons/ellipsis';
  import GaugeIcon from '@lucide/svelte/icons/gauge';
  import GlobeIcon from '@lucide/svelte/icons/globe';
  import ListTodoIcon from '@lucide/svelte/icons/list-todo';
  import MemoryStickIcon from '@lucide/svelte/icons/memory-stick';
  import RefreshCwIcon from '@lucide/svelte/icons/refresh-cw';
  import SearchIcon from '@lucide/svelte/icons/search';
  import ServerIcon from '@lucide/svelte/icons/server';
  import ShieldCheckIcon from '@lucide/svelte/icons/shield-check';
  import Trash2Icon from '@lucide/svelte/icons/trash-2';
  import UploadIcon from '@lucide/svelte/icons/upload';
  import XIcon from '@lucide/svelte/icons/x';

  import SystemIcon from './SystemIcon.svelte';
  import { isActingOnNode } from './is-acting.js';

  type Filter = 'all' | 'online' | 'offline' | 'pending';
  type NodeRecord = Record<string, any>;

  type Props = {
    text: Record<string, string>;
    dashboard?: {
      nodes?: NodeRecord[];
      totals?: Record<string, number>;
    };
    loading: boolean;
    filter?: Filter;
    actingId?: string | undefined;
    approve: (node: NodeRecord) => void | Promise<void>;
    reject: (node: NodeRecord) => void | Promise<void>;
    testBackend: (node: NodeRecord) => void | Promise<void>;
    deleteBackend: (node: NodeRecord) => void | Promise<void>;
    refresh: () => void | Promise<void>;
    copyToClipboard?: (value: string, success?: string) => void | Promise<void>;
  };

  let {
    text,
    dashboard,
    loading,
    filter = $bindable<Filter>('all'),
    actingId,
    approve,
    reject,
    testBackend,
    deleteBackend,
    refresh,
    copyToClipboard,
  }: Props = $props();

  let search = $state('');
  let inspectedNode = $state<NodeRecord | undefined>();
  let sheetOpen = $state(false);

  const allNodes = $derived(dashboard?.nodes ?? []);
  const visibleNodes = $derived(
    allNodes.filter((node) => {
      const statusMatches =
        filter === 'all' ||
        (filter === 'online' && node.online) ||
        (filter === 'offline' && !node.online) ||
        node.registration?.status === filter;
      const query = search.trim().toLowerCase();
      if (!query) return statusMatches;
      const haystack = [
        node.registration?.name,
        node.registration?.backendId,
        node.registration?.ip,
        ...(node.registration?.ips ?? []),
        node.registration?.location,
        ...(node.registration?.tags ?? []),
      ]
        .filter(Boolean)
        .join(' ')
        .toLowerCase();
      return statusMatches && haystack.includes(query);
    }),
  );
  const onlineCount = $derived(allNodes.filter((node) => node.online).length);
  const offlineCount = $derived(allNodes.filter((node) => !node.online).length);
  const pendingCount = $derived(
    allNodes.filter((node) => node.registration?.status === 'pending').length,
  );
  function label(key: string, fallback: string) {
    return text[key] ?? fallback;
  }

  function nodeStatus(node: NodeRecord): string {
    const status = node.registration?.status;
    if (status === 'pending') return label('pending', 'Pending');
    if (status === 'approved') return label('approved', 'Approved');
    return label('rejected', 'Rejected');
  }

  function ipValues(node: NodeRecord): string[] {
    const registration = node.registration ?? {};
    const values =
      Array.isArray(registration.ips) && registration.ips.length
        ? registration.ips
        : registration.ip
          ? [registration.ip]
          : [];
    return values.filter(
      (value: unknown): value is string => typeof value === 'string' && value.length > 0,
    );
  }

  function ips(node: NodeRecord): string {
    const values = ipValues(node);
    return values.join(', ') || '—';
  }

  function locationRegionCode(node: NodeRecord): string | undefined {
    const location = node.registration?.location;
    if (typeof location !== 'string') return undefined;
    const parts = location.split(',');
    const candidate = parts[parts.length - 1]?.trim().toUpperCase();
    if (!candidate || !/^[A-Z]{2}$/.test(candidate)) return undefined;
    try {
      return new Intl.DisplayNames(['en'], { type: 'region', fallback: 'none' }).of(candidate)
        ? candidate
        : undefined;
    } catch {
      return undefined;
    }
  }

  function cpuWidth(node: NodeRecord) {
    return Math.min(100, Math.max(0, node.status?.metrics?.cpu?.usagePercent ?? 0));
  }

  function cpu(node: NodeRecord) {
    const value = node.status?.metrics?.cpu?.usagePercent;
    return typeof value === 'number' ? `${Math.round(value)}%` : '—';
  }

  function cpuCores(node: NodeRecord) {
    const cores = node.status?.metrics?.cpu?.cores;
    return typeof cores === 'number' && cores > 0 ? `${cores}C` : '—';
  }

  function memoryWidth(node: NodeRecord) {
    const memory = node.status?.metrics?.memory;
    return memory?.totalBytes
      ? Math.min(100, Math.max(0, (memory.usedBytes / memory.totalBytes) * 100))
      : 0;
  }

  function memory(node: NodeRecord) {
    const width = memoryWidth(node);
    return width ? `${Math.round(width)}%` : '—';
  }

  function memoryCapacity(node: NodeRecord) {
    const bytes = node.status?.metrics?.memory?.totalBytes;
    if (typeof bytes !== 'number' || bytes <= 0) return '—';
    const gib = bytes / 1024 ** 3;
    if (gib >= 10) return `${Math.round(gib)} GB`;
    if (gib >= 1) return `${gib.toFixed(1).replace(/\.0$/, '')} GB`;
    return `${Math.max(1, Math.round(bytes / 1024 ** 2))} MB`;
  }

  function queue(node: NodeRecord) {
    const metrics = node.status?.metrics?.queue;
    return (metrics?.waiting ?? 0) + (metrics?.active ?? 0);
  }

  function queueWidth(node: NodeRecord) {
    return Math.min(100, queue(node) * 10);
  }

  function systemSummary(node: NodeRecord) {
    const system = node.status?.system;
    if (!system) return label('unavailable', 'Unavailable');
    const distribution = [system.distribution, system.version].filter(Boolean).join(' ');
    return distribution || [system.platform, system.architecture].filter(Boolean).join(' · ');
  }

  function systemDescription(node: NodeRecord) {
    const system = node.status?.system;
    if (!system) return label('unavailable', 'Unavailable');
    return [system.distribution, system.version, system.kernel, system.architecture]
      .filter(Boolean)
      .join(' · ');
  }

  function byteRate(value: unknown) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0)
      return label('unavailable', 'Unavailable');
    const units = ['B/s', 'KiB/s', 'MiB/s', 'GiB/s'];
    let amount = value;
    let unit = 0;
    while (amount >= 1024 && unit < units.length - 1) {
      amount /= 1024;
      unit += 1;
    }
    const display = amount >= 10 || unit === 0 ? Math.round(amount) : amount.toFixed(1);
    return `${display} ${units[unit]}`;
  }

  function receivedRate(node: NodeRecord) {
    return byteRate(node.status?.metrics?.network?.receivedBytesPerSecond);
  }

  function transmittedRate(node: NodeRecord) {
    return byteRate(node.status?.metrics?.network?.transmittedBytesPerSecond);
  }

  function isActing(node: NodeRecord) {
    return isActingOnNode(actingId, node);
  }

  function inspect(node: NodeRecord) {
    inspectedNode = node;
    sheetOpen = true;
  }

  function tunnelLabel(node: NodeRecord) {
    return node.registration?.baseUrl?.includes('trycloudflare.com')
      ? label('temporary', 'Temporary')
      : label('stable', 'Stable');
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

<section class="fleet-view" aria-label={label('nodes', 'Nodes')}>
  <div class="fleet-toolbar">
    <label class="field-shell search-shell">
      <SearchIcon />
      <span class="sr-only">{label('searchLabel', 'Search nodes')}</span>
      <input
        type="search"
        bind:value={search}
        placeholder={label('searchPlaceholder', 'Search name, IP, region, or tag')}
      />
    </label>
    <div class="status-segment" role="group" aria-label={label('filter', 'Status filter')}>
      <Button
        variant="ghost"
        class={`segment-button ${filter === 'all' ? 'active' : ''}`}
        aria-pressed={filter === 'all'}
        onclick={() => (filter = 'all')}
      >
        <span>{label('all', 'All')}</span><span class="segment-count">{allNodes.length}</span>
      </Button>
      <Button
        variant="ghost"
        class={`segment-button ${filter === 'online' ? 'active' : ''}`}
        aria-pressed={filter === 'online'}
        onclick={() => (filter = 'online')}
      >
        <span>{label('online', 'Online')}</span><span class="segment-count">{onlineCount}</span>
      </Button>
      <Button
        variant="ghost"
        class={`segment-button ${filter === 'pending' ? 'active' : ''}`}
        aria-pressed={filter === 'pending'}
        onclick={() => (filter = 'pending')}
      >
        <span>{label('pending', 'Pending')}</span><span class="segment-count">{pendingCount}</span>
      </Button>
      <Button
        variant="ghost"
        class={`segment-button ${filter === 'offline' ? 'active' : ''}`}
        aria-pressed={filter === 'offline'}
        onclick={() => (filter = 'offline')}
      >
        <span>{label('offline', 'Offline')}</span><span class="segment-count">{offlineCount}</span>
      </Button>
    </div>
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

  {#if loading && !dashboard}
    <div class="nodes-grid" aria-busy="true">
      {#each Array(6) as _}
        <Card.Root class="fleet-card skeleton-card">
          <div class="skeleton-head">
            <Skeleton class="h-4 w-2/3" /><Skeleton class="size-8 rounded-xl" />
          </div>
          <Skeleton class="mt-3 h-3 w-1/3" />
          <Skeleton class="mt-6 h-12 w-full" />
          <div class="metric-skeletons">
            <Skeleton class="h-16" /><Skeleton class="h-16" /><Skeleton class="h-16" />
          </div>
        </Card.Root>
      {/each}
    </div>
  {:else if visibleNodes.length === 0}
    <div class="empty-state">
      <div>
        <span class="empty-icon"><SearchIcon /></span>
        <h2>{label('noNodes', 'No matching nodes')}</h2>
        <p>{label('noNodesHint', 'Adjust the search query or status filter, then try again.')}</p>
      </div>
    </div>
  {:else}
    <div class="nodes-grid" aria-live="polite">
      {#each visibleNodes as node, index (node.registration.id)}
        <Card.Root
          class={`fleet-card ${node.registration.status === 'pending' ? 'pending-card' : ''}`}
          style={`--i: ${index}`}
        >
          {@const regionCode = locationRegionCode(node)}
          <div class="node-card-head">
            <span
              class:online={node.online}
              class="status-orb"
              role="img"
              aria-label={node.online ? label('online', 'Online') : label('offline', 'Offline')}
            ></span>
            <div class="node-identity">
              <div class="node-identity-row">
                <h2 title={node.registration.name}>{node.registration.name}</h2>
                <span class="identity-separator" aria-hidden="true">·</span>
                <span
                  class="node-location"
                  title={node.registration.location ?? label('unavailable', 'Location unavailable')}
                >
                  <span class="location-group">
                    {#if regionCode}
                      <span
                        class={`country-flag fi fi-${regionCode.toLowerCase()}`}
                        title={regionCode}
                        aria-hidden="true"
                      ></span>
                    {:else}
                      <GlobeIcon aria-hidden="true" />
                    {/if}
                    <span class="location-name"
                      >{node.registration.location ??
                        label('unavailable', 'Location unavailable')}</span
                    >
                  </span>
                </span>
              </div>
            </div>
            <div class="node-card-actions">
              <span
                class={`approval-mark ${node.registration.status}`}
                role="img"
                aria-label={nodeStatus(node)}
                title={nodeStatus(node)}
              >
                {#if node.registration.status === 'approved'}
                  <ShieldCheckIcon />
                {:else if node.registration.status === 'rejected'}
                  <XIcon />
                {:else}
                  <i aria-hidden="true"></i>
                {/if}
              </span>
              <DropdownMenu.Root>
                <DropdownMenu.Trigger>
                  {#snippet child({ props })}
                    <Button
                      {...props}
                      variant="ghost"
                      size="icon"
                      class="node-overflow-button size-11 rounded-full"
                      aria-label={label('actions', 'Node actions')}
                    >
                      <EllipsisIcon />
                    </Button>
                  {/snippet}
                </DropdownMenu.Trigger>
                <DropdownMenu.Content align="end" class="node-menu">
                  <DropdownMenu.Item onclick={() => inspect(node)}
                    ><ServerIcon />{label('inspect', 'Inspect node')}</DropdownMenu.Item
                  >
                  <DropdownMenu.Item
                    onclick={() => copyToClipboard?.(ips(node), label('copied', 'Copied'))}
                    ><CopyIcon />{label('copyIp', 'Copy public IP')}</DropdownMenu.Item
                  >
                  {#if node.backend}
                    <DropdownMenu.Item disabled={isActing(node)} onclick={() => testBackend(node)}
                      ><GaugeIcon />{label('test', 'Test connection')}</DropdownMenu.Item
                    >
                  {/if}
                  <DropdownMenu.Separator />
                  <DropdownMenu.Item
                    class="danger-menu-item"
                    disabled={isActing(node)}
                    onclick={() => deleteBackend(node)}
                    ><Trash2Icon />{label('remove', 'Remove node + Tunnel')}</DropdownMenu.Item
                  >
                </DropdownMenu.Content>
              </DropdownMenu.Root>
            </div>
          </div>

          <ul class="node-ip-list" aria-label={label('ip', 'IP')}>
            {#each ipValues(node) as ip, ipIndex (ipIndex)}
              <li class="node-ip">{ip}</li>
            {:else}
              <li class="node-ip">—</li>
            {/each}
          </ul>

          <dl class="metrics">
            <div class="metric">
              <dt class="metric-label">
                <CpuIcon aria-hidden="true" />
                <span class="sr-only">{label('cpu', 'CPU')}</span>
              </dt>
              <dd class="metric-reading"><strong>{cpuCores(node)}</strong><em>{cpu(node)}</em></dd>
              <div class="meter"><i style={`--meter: ${cpuWidth(node)}%`}></i></div>
            </div>
            <div class="metric">
              <dt class="metric-label">
                <MemoryStickIcon aria-hidden="true" />
                <span class="sr-only">{label('memory', 'Memory')}</span>
              </dt>
              <dd class="metric-reading">
                <strong>{memoryCapacity(node)}</strong><em>{memory(node)}</em>
              </dd>
              <div class="meter"><i style={`--meter: ${memoryWidth(node)}%`}></i></div>
            </div>
            <div class="metric">
              <dt class="metric-label">
                <ListTodoIcon aria-hidden="true" />
                <span class="sr-only">{label('queue', 'Queue')}</span>
              </dt>
              <dd class="metric-reading">
                <strong>{queue(node)}</strong><em>{label('jobs', 'jobs')}</em>
              </dd>
              <div class="meter">
                <i class:queue-hot={queue(node) > 3} style={`--meter: ${queueWidth(node)}%`}></i>
              </div>
            </div>
          </dl>

          <div
            class="node-secondary-details"
            aria-label={label('cachedTelemetry', 'Cached telemetry')}
          >
            <span
              class="secondary-detail system-detail"
              title={`${label('system', 'System')}: ${systemDescription(node)}`}
            >
              <SystemIcon distribution={node.status?.system?.distribution} />
              <span class="sr-only">{label('system', 'System')}</span>
              <span>{systemSummary(node)}</span>
            </span>
            <span
              class="secondary-detail network-detail"
              title={`${label('download', 'Download')}: ${receivedRate(node)}`}
            >
              <DownloadIcon aria-hidden="true" />
              <span class="sr-only">{label('download', 'Download')}</span>
              <span>{receivedRate(node)}</span>
            </span>
            <span
              class="secondary-detail network-detail"
              title={`${label('upload', 'Upload')}: ${transmittedRate(node)}`}
            >
              <UploadIcon aria-hidden="true" />
              <span class="sr-only">{label('upload', 'Upload')}</span>
              <span>{transmittedRate(node)}</span>
            </span>
          </div>

          {#if (node.registration.tags?.length ?? 0) > 0 || node.registration.status === 'pending'}
            <footer class="node-footer">
              {#if (node.registration.tags?.length ?? 0) > 0}
                <div class="node-context">
                  {#each node.registration.tags ?? [] as tag}<Badge variant="outline">{tag}</Badge
                    >{/each}
                </div>
              {/if}
              {#if node.registration.status === 'pending'}
                <div class="pending-actions">
                  <Button
                    variant="outline"
                    class="reject-button"
                    disabled={isActing(node)}
                    onclick={() => reject(node)}
                  >
                    <XIcon />{label('reject', 'Reject')}
                  </Button>
                  <Button
                    class="approve-button"
                    disabled={isActing(node)}
                    onclick={() => approve(node)}
                  >
                    <CheckIcon />{label('approve', 'Approve')}
                  </Button>
                </div>
              {/if}
            </footer>
          {/if}
        </Card.Root>
      {/each}
    </div>
  {/if}
</section>

<Sheet.Root bind:open={sheetOpen}>
  <Sheet.Content side="right" class="inspection-sheet" showCloseButton={false}>
    {#if inspectedNode}
      <Sheet.Header class="sheet-heading">
        <div class="sheet-title-row">
          <span class:online={inspectedNode.online} class="status-orb"></span>
          <div>
            <Sheet.Title>{inspectedNode.registration.name}</Sheet.Title>
            <Sheet.Description class="sheet-description">
              <span>{inspectedNode.registration.location ?? '—'}</span><span aria-hidden="true"
                >·</span
              ><span class="sheet-ip-list" role="list" aria-label={label('ip', 'IP')}>
                {#each ipValues(inspectedNode) as ip, ipIndex (ipIndex)}
                  <span class="sheet-ip" role="listitem">{ip}</span>
                {:else}
                  <span class="sheet-ip" role="listitem">—</span>
                {/each}
              </span>
            </Sheet.Description>
            >
          </div>
          <Sheet.Close>
            {#snippet child({ props })}
              <Button
                {...props}
                variant="ghost"
                size="icon-sm"
                class="size-11 rounded-xl"
                aria-label={label('close', 'Close')}><XIcon /></Button
              >
            {/snippet}
          </Sheet.Close>
        </div>
      </Sheet.Header>

      <div class="sheet-body">
        <section class="sheet-section">
          <h3>{label('runtime', 'Runtime metrics')}</h3>
          <div class="runtime-chart">
            <div>
              <span>{label('cpu', 'CPU')}</span>
              <div class="chart-track"><i style={`--meter: ${cpuWidth(inspectedNode)}%`}></i></div>
              <strong>{cpu(inspectedNode)}</strong>
            </div>
            <div>
              <span>{label('memory', 'Memory')}</span>
              <div class="chart-track">
                <i style={`--meter: ${memoryWidth(inspectedNode)}%`}></i>
              </div>
              <strong>{memory(inspectedNode)}</strong>
            </div>
            <div>
              <span>{label('queue', 'Queue')}</span>
              <div class="chart-track">
                <i style={`--meter: ${queueWidth(inspectedNode)}%`}></i>
              </div>
              <strong>{queue(inspectedNode)}</strong>
            </div>
          </div>
        </section>
        <section class="sheet-section">
          <h3>{label('identity', 'Agent identity')}</h3>
          <dl class="detail-grid">
            <div>
              <dt>ID</dt>
              <dd>{inspectedNode.registration.backendId}</dd>
            </div>
            <div>
              <dt>{label('tunnelMode', 'Tunnel')}</dt>
              <dd>{tunnelLabel(inspectedNode)}</dd>
            </div>
            <div>
              <dt>{label('version', 'Version')}</dt>
              <dd>{inspectedNode.status?.health?.version ?? '—'}</dd>
            </div>
            <div>
              <dt>{label('lastSeen', 'Last checked')}</dt>
              <dd>{relativeTime(inspectedNode.checkedAt)}</dd>
            </div>
          </dl>
        </section>
        <section class="sheet-section">
          <h3>{label('activity', 'Recent activity')}</h3>
          <div class="activity-row">
            <ActivityIcon />{inspectedNode.online
              ? label('online', 'Online')
              : label('offline', 'Offline')} · {relativeTime(inspectedNode.checkedAt)}
          </div>
        </section>
      </div>

      <Sheet.Footer class="sheet-footer">
        <Sheet.Close>
          {#snippet child({ props })}<Button
              {...props}
              variant="outline"
              class="h-11 rounded-[10px]">{label('close', 'Close')}</Button
            >{/snippet}
        </Sheet.Close>
        {#if inspectedNode.backend}
          <Button
            class="h-11 rounded-[10px]"
            disabled={isActing(inspectedNode)}
            onclick={() => inspectedNode && testBackend(inspectedNode)}
            ><ShieldCheckIcon />{label('test', 'Test')}</Button
          >
        {/if}
      </Sheet.Footer>
    {/if}
  </Sheet.Content>
</Sheet.Root>

<style>
  .fleet-view {
    animation: view-in 240ms cubic-bezier(0.23, 1, 0.32, 1) both;
  }
  .fleet-toolbar {
    display: grid;
    grid-template-columns: minmax(16rem, 1fr) auto 2.75rem;
    align-items: center;
    gap: 0.625rem;
    margin-bottom: 1.25rem;
  }
  .field-shell {
    height: 2.75rem;
    display: flex;
    align-items: center;
    border: 1px solid color-mix(in oklch, var(--border) 76%, transparent);
    border-radius: 0.875rem;
    background: var(--surface-soft);
    box-shadow:
      inset 0 1px 0 color-mix(in oklch, var(--surface) 72%, transparent),
      0 1px 2px color-mix(in oklch, var(--foreground) 3%, transparent);
    transition:
      background-color 160ms cubic-bezier(0.23, 1, 0.32, 1),
      border-color 160ms cubic-bezier(0.23, 1, 0.32, 1),
      box-shadow 160ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  .field-shell:focus-within {
    border-color: color-mix(in oklch, var(--primary) 55%, var(--border));
    background: var(--card);
    box-shadow:
      0 0 0 3px color-mix(in oklch, var(--primary) 18%, transparent),
      0 1px 2px color-mix(in oklch, var(--foreground) 4%, transparent);
  }
  .search-shell {
    flex: 1;
    max-width: 32rem;
    gap: 0.625rem;
    padding: 0 0.8125rem;
    color: var(--muted-foreground);
  }
  .search-shell :global(svg) {
    width: 1rem;
    height: 1rem;
    flex: none;
  }
  .search-shell input {
    width: 100%;
    min-width: 0;
    outline: 0;
    border: 0;
    color: var(--foreground);
    background: transparent;
  }
  .search-shell input:focus,
  .search-shell input:focus-visible {
    outline: none;
    box-shadow: none;
  }
  .search-shell input::placeholder {
    color: var(--muted-foreground);
  }
  .status-segment {
    display: flex;
    align-items: center;
    gap: 0.1875rem;
    min-width: 0;
    padding: 0.1875rem;
    border: 1px solid color-mix(in oklch, var(--border) 76%, transparent);
    border-radius: 0.875rem;
    background: var(--surface-soft);
    box-shadow: inset 0 1px 0 color-mix(in oklch, var(--surface) 70%, transparent);
  }
  :global(.segment-button) {
    min-width: 0;
    height: 2.375rem;
    gap: 0.375rem;
    padding-inline: 0.75rem;
    border-radius: 0.6875rem;
    color: var(--muted-foreground);
    font-size: 0.75rem;
    box-shadow: none;
  }
  :global(.segment-button:hover) {
    color: var(--foreground);
    background: color-mix(in oklch, var(--card) 64%, transparent);
  }
  :global(.segment-button.active) {
    color: var(--foreground);
    background: var(--card);
    box-shadow:
      0 1px 2px color-mix(in oklch, var(--foreground) 8%, transparent),
      0 3px 8px color-mix(in oklch, var(--foreground) 5%, transparent);
  }
  .segment-count {
    color: color-mix(in oklch, currentColor 68%, transparent);
    font-family: var(--font-mono);
    font-size: 0.625rem;
    font-variant-numeric: tabular-nums;
  }
  :global(.refresh-button) {
    flex: none;
    color: var(--foreground-soft);
    background: var(--surface-soft);
    box-shadow: inset 0 1px 0 color-mix(in oklch, var(--surface) 70%, transparent);
  }
  :global(.refresh-button:hover) {
    color: var(--foreground);
    background: var(--card);
  }
  :global(.refresh-button svg) {
    width: 0.95rem;
    height: 0.95rem;
  }
  :global(.spin) {
    animation: spin 0.8s linear infinite;
  }
  .nodes-grid {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 1.125rem;
  }
  :global(.fleet-card) {
    --fleet-card-inset: 0.875rem;
    min-width: 0;
    gap: 0;
    padding: var(--fleet-card-inset);
    border: 1px solid color-mix(in oklch, var(--border) 82%, transparent);
    border-radius: 1.375rem;
    background: var(--card);
    box-shadow:
      0 1px 2px color-mix(in oklch, var(--foreground) 4%, transparent),
      0 10px 30px color-mix(in oklch, var(--foreground) 4%, transparent);
    transition:
      transform 180ms cubic-bezier(0.23, 1, 0.32, 1),
      box-shadow 180ms cubic-bezier(0.23, 1, 0.32, 1),
      border-color 180ms cubic-bezier(0.23, 1, 0.32, 1);
    animation: card-in 300ms cubic-bezier(0.23, 1, 0.32, 1) both;
    animation-delay: calc(var(--i, 0) * 45ms);
  }
  :global(.fleet-card:hover) {
    border-color: color-mix(in oklch, var(--border) 70%, var(--foreground) 30%);
    box-shadow:
      0 2px 4px color-mix(in oklch, var(--foreground) 5%, transparent),
      0 16px 38px color-mix(in oklch, var(--foreground) 7%, transparent);
    transform: translateY(-2px);
  }
  :global(.fleet-card.pending-card) {
    border-color: color-mix(in oklch, var(--warning) 34%, var(--border));
  }
  .node-card-head {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .status-orb {
    position: relative;
    width: 0.5rem;
    height: 0.5rem;
    flex: none;
    border-radius: 999px;
    background: var(--muted-foreground);
    box-shadow: 0 0 0 4px var(--muted);
  }
  .status-orb.online {
    background: oklch(59% 0.15 152);
    box-shadow: 0 0 0 4px oklch(95.5% 0.035 152);
  }
  .status-orb.online::after {
    position: absolute;
    inset: -0.25rem;
    border: 1px solid oklch(59% 0.15 152);
    border-radius: inherit;
    content: '';
    animation: live 2.2s ease-out infinite;
  }
  :global(.dark) .status-orb.online {
    box-shadow: 0 0 0 4px oklch(26% 0.05 152);
  }
  .node-identity {
    min-width: 0;
    flex: 1;
  }
  .node-identity-row {
    display: flex;
    align-items: center;
    min-width: 0;
    overflow: hidden;
    gap: 0.375rem;
  }
  .node-identity h2 {
    min-width: 0;
    max-width: 58%;
    flex: 0 1 auto;
    overflow: hidden;
    margin: 0;
    color: var(--foreground);
    font-family: var(--font-mono);
    font-size: 1rem;
    font-weight: 650;
    letter-spacing: -0.015em;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .identity-separator {
    flex: 0 0 auto;
    color: color-mix(in oklch, var(--muted-foreground) 70%, transparent);
    font-size: 0.75rem;
    line-height: 1;
  }
  .node-ip-list {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3125rem;
    margin: 0.5rem 0 0;
    padding: 0;
    list-style: none;
  }
  .node-ip {
    display: inline-flex;
    width: fit-content;
    max-width: 100%;
    overflow-x: auto;
    padding: 0.1875rem 0.4375rem;
    border: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
    border-radius: 999px;
    color: var(--foreground-soft);
    background: color-mix(in oklch, var(--surface-soft) 68%, transparent);
    font-family: var(--font-mono);
    font-size: 0.6875rem;
    font-variant-numeric: tabular-nums;
    line-height: 1.25;
    scrollbar-width: none;
    white-space: nowrap;
  }
  .node-ip::-webkit-scrollbar {
    display: none;
  }
  .node-location {
    display: inline-flex;
    align-items: center;
    min-width: 0;
    max-width: 38%;
    flex: 0 1 auto;
    gap: 0.3125rem;
    margin: 0;
    color: var(--muted-foreground);
    font-size: 0.6875rem;
  }
  .node-location :global(svg) {
    width: 0.75rem;
    height: 0.75rem;
    flex: none;
  }
  .location-group {
    display: inline-flex;
    align-items: center;
    min-width: 0;
    max-width: 100%;
    gap: 0.3125rem;
  }
  .country-flag {
    display: inline-block;
    flex: 0 0 auto;
    line-height: 1;
    font-size: 0.875rem;
  }
  .location-name {
    min-width: 0;
    flex: 1 1 auto;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .node-card-actions {
    display: flex;
    align-items: center;
    gap: 0.25rem;
    flex: none;
  }
  .approval-mark {
    width: 1.75rem;
    height: 1.75rem;
    display: grid;
    place-items: center;
    flex: none;
    border: 1px solid color-mix(in oklch, var(--border) 76%, transparent);
    border-radius: 999px;
    color: var(--muted-foreground);
    background: var(--surface-soft);
  }
  .approval-mark :global(svg) {
    width: 0.8125rem;
    height: 0.8125rem;
  }
  .approval-mark.approved {
    border-color: color-mix(in oklch, var(--success) 30%, var(--border));
    color: oklch(45% 0.12 152);
    background: var(--success-soft);
  }
  .approval-mark.rejected {
    border-color: color-mix(in oklch, var(--destructive) 30%, var(--border));
    color: var(--destructive);
    background: color-mix(in oklch, var(--destructive) 10%, transparent);
  }
  .approval-mark.pending {
    border-color: color-mix(in oklch, var(--warning) 34%, var(--border));
    background: var(--warning-soft);
  }
  .approval-mark.pending i {
    width: 0.375rem;
    height: 0.375rem;
    border-radius: 999px;
    background: var(--warning);
    box-shadow: 0 0 0 0.1875rem color-mix(in oklch, var(--warning) 18%, transparent);
  }
  :global(.dark) .approval-mark.approved {
    color: oklch(70% 0.14 152);
    background: oklch(26% 0.05 152);
  }
  :global(.node-overflow-button) {
    color: var(--muted-foreground);
  }
  :global(.node-overflow-button:hover) {
    color: var(--foreground);
    background: var(--surface-soft);
  }
  :global(.node-menu) {
    min-width: 12rem;
    opacity: 1;
    border-radius: 0.75rem;
    transform: scale(1);
    transform-origin: var(--bits-dropdown-menu-content-transform-origin);
    box-shadow:
      0 2px 6px color-mix(in oklch, var(--foreground) 10%, transparent),
      0 22px 46px color-mix(in oklch, var(--foreground) 16%, transparent);
    transition:
      opacity 140ms cubic-bezier(0.23, 1, 0.32, 1),
      transform 160ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  :global(.node-menu[data-state='open'][data-starting-style]),
  :global(.node-menu[data-state='closed'][data-ending-style]) {
    opacity: 0;
    transform: scale(0.98);
  }
  :global(.node-menu [data-slot='dropdown-menu-item']) {
    min-height: 2.75rem;
    border-radius: 0.5rem;
  }
  :global(.danger-menu-item) {
    color: var(--destructive);
  }
  .metrics {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 0;
    margin: 0.625rem 0 0;
    padding-top: 0.625rem;
    border-top: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
  }
  .metric {
    position: relative;
    min-width: 0;
    padding: 0.0625rem 0.5625rem 0.125rem;
  }
  .metric + .metric {
    border-left: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
  }
  .metric:first-child {
    padding-left: 0;
  }
  .metric:last-child {
    padding-right: 0;
  }
  .metric-label {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 1.375rem;
    height: 1.375rem;
    color: var(--muted-foreground);
  }
  .metric-label :global(svg) {
    width: 0.875rem;
    height: 0.875rem;
    stroke-width: 1.7;
  }
  .metric-reading {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 0.25rem;
    margin: 0;
  }
  .metric strong {
    display: block;
    overflow: hidden;
    color: var(--foreground);
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 0.8125rem;
    font-variant-numeric: tabular-nums;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .metric em {
    flex: none;
    color: var(--foreground-soft);
    font-family: var(--font-mono);
    font-size: 0.625rem;
    font-style: normal;
    font-variant-numeric: tabular-nums;
  }
  .meter,
  .chart-track {
    height: 0.1875rem;
    margin-top: 0.3125rem;
    overflow: hidden;
    border-radius: 999px;
    background: color-mix(in oklch, var(--muted-foreground) 18%, transparent);
  }
  .meter i,
  .chart-track i {
    display: block;
    width: var(--meter);
    height: 100%;
    border-radius: inherit;
    background: color-mix(in oklch, var(--foreground) 62%, var(--muted-foreground));
    opacity: 0.72;
  }
  .meter i.queue-hot {
    background: oklch(64% 0.14 75);
    opacity: 0.95;
  }
  .node-secondary-details {
    display: grid;
    grid-template-columns: minmax(0, 1fr) max-content max-content;
    align-items: center;
    column-gap: 0.625rem;
    min-width: 0;
    margin-top: 0.5rem;
    padding-block: 0.5rem;
    border-top: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
    color: var(--muted-foreground);
  }
  .node-secondary-details:last-child {
    margin-bottom: calc(-1 * var(--fleet-card-inset));
  }
  .secondary-detail {
    display: inline-flex;
    align-items: center;
    min-height: 1rem;
    min-width: 0;
    gap: 0.3125rem;
    font-size: 0.625rem;
    line-height: 1rem;
  }
  .secondary-detail :global(svg) {
    display: block;
    width: 0.75rem;
    height: 0.75rem;
    flex: 0 0 0.75rem;
    stroke-width: 1.75;
  }
  .secondary-detail > span:last-child {
    min-width: 0;
    line-height: inherit;
  }
  .system-detail {
    min-width: 0;
  }
  .system-detail span:last-child {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .network-detail {
    color: var(--foreground-soft);
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .node-footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    min-height: 0;
    margin-top: 0;
    padding-top: 0.5rem;
    border-top: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
  }
  .node-context,
  .pending-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.3125rem;
    min-width: 0;
  }
  .node-context :global([data-slot='badge']) {
    max-width: 6rem;
    overflow: hidden;
    min-height: 1.5rem;
    padding-inline: 0.5rem;
    border-radius: 999px;
    color: var(--muted-foreground);
    font-size: 0.625rem;
    text-overflow: ellipsis;
  }
  :global(.approve-button) {
    min-height: 2.75rem;
    border-radius: 0.75rem;
    box-shadow:
      0 1px 2px color-mix(in oklch, var(--primary) 22%, transparent),
      0 7px 16px color-mix(in oklch, var(--primary) 18%, transparent);
  }
  :global(.reject-button) {
    min-height: 2.75rem;
    color: var(--destructive);
    border-radius: 0.75rem;
  }
  .empty-state {
    min-height: 20.625rem;
    display: grid;
    place-items: center;
    padding: 2.5rem;
    border: 1px dashed var(--border);
    border-radius: 1rem;
    color: var(--muted-foreground);
    background: color-mix(in oklch, var(--card) 60%, transparent);
    text-align: center;
  }
  .empty-icon {
    width: 3rem;
    height: 3rem;
    display: grid;
    place-items: center;
    margin: 0 auto 1rem;
    border-radius: 0.875rem;
    color: var(--muted-foreground);
    background: var(--muted);
  }
  .empty-icon :global(svg) {
    width: 1.25rem;
    height: 1.25rem;
  }
  .empty-state h2 {
    margin: 0 0 0.375rem;
    color: var(--foreground);
    font-size: 1.0625rem;
  }
  .empty-state p {
    max-width: 28rem;
    margin: 0;
  }
  :global(.skeleton-card) {
    animation: none;
  }
  .skeleton-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .metric-skeletons {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 0.5rem;
    margin-top: 1rem;
  }
  :global(.inspection-sheet) {
    width: min(100%, 29rem);
    padding: 0;
  }
  :global(.sheet-heading) {
    padding: 1.5rem;
    border-bottom: 1px solid var(--border);
  }
  .sheet-title-row {
    display: flex;
    align-items: flex-start;
    gap: 0.75rem;
  }
  .sheet-title-row > div {
    min-width: 0;
    flex: 1;
  }
  .sheet-title-row :global([data-slot='sheet-title']) {
    overflow: hidden;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  :global(.sheet-description) {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.3125rem;
  }
  .sheet-ip-list {
    display: inline-flex;
    flex-wrap: wrap;
    gap: 0.25rem;
    min-width: 0;
  }
  .sheet-ip {
    max-width: 100%;
    padding: 0.0625rem 0.3125rem;
    border: 1px solid color-mix(in oklch, var(--border) 72%, transparent);
    border-radius: 0.3125rem;
    color: var(--foreground-soft);
    background: var(--surface-soft);
    font-family: var(--font-mono);
    font-size: 0.6875rem;
    font-variant-numeric: tabular-nums;
    overflow-wrap: anywhere;
  }
  .sheet-body {
    flex: 1;
    overflow: auto;
    padding: 1.5rem;
  }
  .sheet-section + .sheet-section {
    margin-top: 1.75rem;
    padding-top: 1.75rem;
    border-top: 1px solid var(--border);
  }
  .sheet-section h3 {
    margin: 0 0 0.875rem;
    font-size: 0.8125rem;
    font-weight: 650;
    letter-spacing: 0.02em;
  }
  .runtime-chart {
    display: grid;
    gap: 0.875rem;
  }
  .runtime-chart > div {
    display: grid;
    grid-template-columns: 4rem 1fr 2.5rem;
    align-items: center;
    gap: 0.75rem;
    color: var(--muted-foreground);
    font-size: 0.8125rem;
  }
  .runtime-chart .chart-track {
    margin: 0;
  }
  .runtime-chart strong {
    color: var(--foreground);
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 0.8125rem;
    text-align: right;
  }
  .detail-grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.75rem;
    margin: 0;
  }
  .detail-grid > div {
    min-width: 0;
    padding: 0.75rem;
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    background: var(--muted);
  }
  .detail-grid dt {
    margin-bottom: 0.25rem;
    color: var(--muted-foreground);
    font-size: 0.6875rem;
  }
  .detail-grid dd {
    margin: 0;
    overflow: hidden;
    color: var(--foreground);
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 0.75rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .activity-row {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    color: var(--muted-foreground);
    font-size: 0.8125rem;
  }
  .activity-row :global(svg) {
    width: 0.875rem;
    height: 0.875rem;
    color: var(--primary);
  }
  :global(.sheet-footer) {
    display: flex;
    justify-content: end;
    gap: 0.5rem;
    padding: 1rem 1.5rem;
    border-top: 1px solid var(--border);
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
  }
  @keyframes view-in {
    from {
      opacity: 0;
      transform: translateY(5px);
    }
  }
  @keyframes card-in {
    from {
      opacity: 0;
      transform: translateY(10px);
    }
  }
  @keyframes live {
    0% {
      opacity: 0.7;
      transform: scale(0.65);
    }
    75%,
    100% {
      opacity: 0;
      transform: scale(1.35);
    }
  }
  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (max-width: 1120px) {
    .nodes-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
  }
  @media (max-width: 880px) {
    .fleet-toolbar {
      grid-template-columns: minmax(0, 1fr) 2.75rem;
    }
    .search-shell {
      grid-column: 1 / -1;
      max-width: none;
    }
    .status-segment {
      grid-column: 1;
      width: max-content;
      max-width: 100%;
    }
    :global(.refresh-button) {
      grid-column: 2;
    }
  }
  @media (max-width: 700px) {
    .nodes-grid {
      grid-template-columns: 1fr;
    }
  }
  @media (max-width: 520px) {
    .status-segment {
      width: 100%;
    }
    :global(.segment-button) {
      flex: 1;
      padding-inline: 0.5rem;
    }
    .segment-count {
      display: none;
    }
  }
  @media (max-width: 430px) {
    :global(.fleet-card) {
      padding: 0.875rem;
    }
    .node-card-actions {
      gap: 0.125rem;
    }
    .metric {
      padding-inline: 0.375rem;
    }
    .metric:first-child {
      padding-left: 0;
    }
    .metric:last-child {
      padding-right: 0;
    }
    .metric-label {
      width: 1.375rem;
      height: 1.375rem;
    }
    .pending-actions {
      width: 100%;
      margin-top: 0.25rem;
    }
    .pending-actions :global(button) {
      flex: 1;
    }
    .node-footer {
      align-items: flex-start;
      flex-direction: column;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .fleet-view,
    :global(.fleet-card),
    .status-orb.online::after,
    :global(.spin) {
      animation: none;
      transition: none;
    }
    :global(.fleet-card:hover) {
      transform: none;
    }
    :global(.node-menu) {
      transition: none;
    }
    :global(.node-menu[data-state='open'][data-starting-style]),
    :global(.node-menu[data-state='closed'][data-ending-style]) {
      opacity: 1;
      transform: none;
    }
  }
</style>
