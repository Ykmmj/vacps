<script lang="ts">
  import { fade, fly } from 'svelte/transition';
  import Check from '@lucide/svelte/icons/check';
  import Clock from '@lucide/svelte/icons/clock';
  import Cloud from '@lucide/svelte/icons/cloud';
  import Copy from '@lucide/svelte/icons/copy';
  import ExternalLink from '@lucide/svelte/icons/external-link';
  import KeyRound from '@lucide/svelte/icons/key-round';
  import LoaderCircle from '@lucide/svelte/icons/loader-circle';
  import RefreshCw from '@lucide/svelte/icons/refresh-cw';
  import Terminal from '@lucide/svelte/icons/terminal';
  import Zap from '@lucide/svelte/icons/zap';
  import { Badge } from '$lib/components/ui/badge/index.js';
  import { Button } from '$lib/components/ui/button/index.js';
  import * as Card from '$lib/components/ui/card/index.js';
  import { Input } from '$lib/components/ui/input/index.js';
  import * as Select from '$lib/components/ui/select/index.js';
  import * as Tabs from '$lib/components/ui/tabs/index.js';
  import * as Tooltip from '$lib/components/ui/tooltip/index.js';
  import { deriveManagedTunnelStage, managedTunnelStageIndex } from './managed-tunnel-stage.js';

  type TunnelMode = 'managed' | 'quick';
  type Zone = { id: string; name: string };
  type ExistingTunnel = {
    tunnelId: string;
    name: string;
    backendId?: string;
    hostname?: string;
    publicUrl?: string;
    bound: boolean;
    boundBackendId?: string;
    deleted: boolean;
  };
  type AsyncAction = () => void | Promise<void>;

  type Props = {
    text: Record<string, string>;
    installBackendName?: string;
    installTags?: string;
    installRedisUrl?: string;
    installAllowApt?: boolean;
    installTunnelMode?: TunnelMode;
    installCommand?: string;
    tokenActive?: boolean;
    tokenExpired?: boolean;
    tokenMsRemaining?: number;
    generatingToken?: boolean;
    generateRegistrationToken: AsyncAction;
    managedProvision?: any;
    provisioningTunnel?: boolean;
    attachingTunnel?: boolean;
    existingTunnels?: ExistingTunnel[];
    selectedExistingTunnelId?: string;
    loadingExistingTunnels?: boolean;
    cloudflareOAuth?: any;
    cloudflareZones?: Zone[];
    selectedCloudflareZone?: string;
    connectingCloudflare?: boolean;
    loadingCloudflareZones?: boolean;
    selectingCloudflareZone?: boolean;
    managedTunnelSetupCommand?: string;
    cloudflareApiTokenGuideUrl?: string;
    connectCloudflare: AsyncAction;
    selectCloudflareZone: (zoneId: string) => void | Promise<void>;
    ensureManagedProvision: AsyncAction;
    loadExistingTunnels: AsyncAction;
    attachExistingTunnel: (tunnelId: string) => void | Promise<void>;
    copyInstallCommand: () => boolean | Promise<boolean>;
    copyToClipboard: (value: string, success?: string) => void | Promise<void>;
  };

  let {
    text,
    installBackendName = $bindable(''),
    installTags = $bindable(''),
    installRedisUrl = $bindable(''),
    installAllowApt = $bindable(false),
    installTunnelMode = $bindable<TunnelMode>('managed'),
    installCommand = '',
    tokenActive = false,
    tokenExpired = false,
    tokenMsRemaining = 0,
    generatingToken = false,
    generateRegistrationToken,
    managedProvision,
    provisioningTunnel = false,
    attachingTunnel = false,
    existingTunnels = [],
    selectedExistingTunnelId = $bindable(''),
    loadingExistingTunnels = false,
    cloudflareOAuth,
    cloudflareZones,
    selectedCloudflareZone = $bindable(''),
    connectingCloudflare = false,
    loadingCloudflareZones = false,
    selectingCloudflareZone = false,
    managedTunnelSetupCommand = '',
    cloudflareApiTokenGuideUrl = '',
    connectCloudflare,
    selectCloudflareZone,
    ensureManagedProvision,
    loadExistingTunnels,
    attachExistingTunnel,
    copyInstallCommand,
    copyToClipboard,
  }: Props = $props();

  let commandCopied = $state(false);

  const tokenLowTime = $derived(tokenActive && tokenMsRemaining <= 60_000);
  const tokenCountdown = $derived(formatCountdown(tokenMsRemaining));

  function formatCountdown(ms: number) {
    const totalSeconds = Math.max(0, Math.ceil(ms / 1000));
    const minutes = Math.floor(totalSeconds / 60);
    const seconds = totalSeconds % 60;
    return `${minutes}:${String(seconds).padStart(2, '0')}`;
  }

  const copy = (value: string, success?: string) => copyToClipboard?.(value, success);
  const label = (key: string, fallback: string) => text?.[key] || fallback;

  function chooseZone(zoneId: string) {
    selectedCloudflareZone = zoneId;
    if (zoneId) void selectCloudflareZone?.(zoneId);
  }

  async function copyCommand() {
    if (await copyInstallCommand?.()) {
      commandCopied = true;
      window.setTimeout(() => (commandCopied = false), 1800);
    }
  }

  const waitingForTunnel = $derived(installTunnelMode === 'managed' && !managedProvision);
  const commandReady = $derived(
    !waitingForTunnel && Boolean(installCommand) && !installCommand.startsWith('#'),
  );
  const managedStage = $derived(deriveManagedTunnelStage(cloudflareOAuth, managedProvision));
  const cloudflareStage = $derived(managedTunnelStageIndex(managedStage));
  const reusableTunnels = $derived(
    (existingTunnels ?? []).filter(
      (tunnel) => !tunnel.deleted && Boolean(tunnel.backendId || tunnel.name),
    ),
  );

  function tunnelOptionLabel(tunnel: ExistingTunnel) {
    const id = tunnel.backendId || tunnel.name;
    const status = tunnel.bound
      ? label('tunnelBound', 'In use')
      : label('tunnelAvailable', 'Available');
    return `${id} · ${status}`;
  }

  function chooseExistingTunnel(tunnelId: string) {
    selectedExistingTunnelId = tunnelId;
    if (tunnelId) void attachExistingTunnel?.(tunnelId);
  }
</script>

<Tooltip.Provider>
  <section
    class="mx-auto w-full max-w-[1480px] animate-in"
    aria-label={label('install', 'Install a node')}
  >
    <div class="grid items-start gap-6 lg:grid-cols-[minmax(0,1fr)_26.25rem]">
      <Card.Root class="rounded-2xl border-border py-0 shadow-surface">
        <Card.Content class="p-5">
          <Tabs.Root bind:value={installTunnelMode} class="gap-0">
            <Tabs.List
              class="tunnel-segment mb-6 grid h-auto w-full grid-cols-2 gap-1 p-1"
              aria-label={label('tunnelMode', 'Tunnel mode')}
            >
              <span
                class:quick={installTunnelMode === 'quick'}
                class="tunnel-selection-indicator"
                aria-hidden="true"
              ></span>
              <Tabs.Trigger value="managed" class="tunnel-option h-11"
                ><Cloud /><span class="tunnel-option-label"
                  >{label('managedTunnel', 'Managed Tunnel')}</span
                ><span class="mode-check" aria-hidden="true"><Check /></span></Tabs.Trigger
              >
              <Tabs.Trigger value="quick" class="tunnel-option h-11"
                ><Zap /><span class="tunnel-option-label"
                  >{label('quickTunnel', 'Quick Tunnel')}</span
                ><span class="mode-check" aria-hidden="true"><Check /></span></Tabs.Trigger
              >
            </Tabs.List>
          </Tabs.Root>

          <div class="grid gap-[14px] sm:grid-cols-2">
            <label class="grid min-w-0 gap-1.5">
              <span class="field-label">{label('nodeName', 'Node name')}</span>
              <Input
                bind:value={installBackendName}
                autocomplete="off"
                placeholder="edge-osaka-01"
                class="h-11 rounded-[10px] border-border bg-background shadow-[0_1px_1px_oklch(20%_.01_250_/_0.04)]"
              />
            </label>
            <label class="grid min-w-0 gap-1.5">
              <span class="field-label">{label('tags', 'Tags')}</span>
              <Input
                bind:value={installTags}
                autocomplete="off"
                placeholder="edge, production, jp"
                class="h-11 rounded-[10px] border-border bg-background shadow-[0_1px_1px_oklch(20%_.01_250_/_0.04)]"
              />
            </label>
            <label class="grid min-w-0 gap-1.5 sm:col-span-2">
              <span class="field-label">{label('redisUrl', 'Redis URL')}</span>
              <Input
                bind:value={installRedisUrl}
                type="password"
                autocomplete="off"
                placeholder="rediss://default:password@host:port"
                class="h-11 rounded-[10px] border-border bg-background font-mono text-xs shadow-[0_1px_1px_oklch(20%_.01_250_/_0.04)]"
              />
            </label>
            <div class="grid min-w-0 gap-1.5 sm:col-span-2" data-od-id="registration-token">
              <span class="field-label">{label('registrationToken', 'Registration token')}</span>
              <div
                class="token-row flex min-h-[3.25rem] items-center gap-3 rounded-[10px] border px-3 py-2.5"
                class:token-row-active={tokenActive}
                class:token-row-expired={tokenExpired}
              >
                <span
                  class="token-mark"
                  class:token-mark-active={tokenActive}
                  class:token-mark-expired={tokenExpired}
                  aria-hidden="true"
                >
                  {#if tokenActive}<Check />{:else}<KeyRound />{/if}
                </span>
                <div class="min-w-0 flex-1">
                  {#if tokenActive}
                    <div class="token-headline">
                      <span class="token-title"
                        >{label('registrationTokenReadyStatus', 'Token ready')}</span
                      >
                      <Badge
                        class="rounded-full bg-emerald-500/12 text-[10px] font-semibold tracking-[0.04em] text-emerald-700 dark:text-emerald-300"
                        >{label('registrationTokenOneTime', 'One-time · 10 min')}</Badge
                      >
                    </div>
                    <p class="token-sub" class:token-sub-low={tokenLowTime}>
                      <Clock class="size-3" aria-hidden="true" />{label(
                        'registrationTokenExpiresIn',
                        'Expires in',
                      )}
                      <span class="token-clock">{tokenCountdown}</span>
                    </p>
                  {:else if tokenExpired}
                    <span class="token-title"
                      >{label('registrationTokenExpired', 'Token expired')}</span
                    >
                  {:else}
                    <p class="token-sub token-sub-idle">
                      {label(
                        'registrationTokenHint',
                        'One-time token, valid for 10 minutes. Never stored by this page.',
                      )}
                    </p>
                  {/if}
                </div>
                {#if tokenActive}
                  <Button
                    variant="ghost"
                    size="icon"
                    class="size-10 shrink-0 rounded-[9px] text-muted-foreground"
                    aria-label={label('regenerateRegistrationToken', 'Generate a replacement')}
                    disabled={generatingToken}
                    onclick={() => generateRegistrationToken?.()}
                  >
                    {#if generatingToken}<LoaderCircle class="animate-spin" />{:else}<RefreshCw
                      />{/if}
                  </Button>
                {:else}
                  <Button
                    variant={tokenExpired ? 'outline' : 'default'}
                    class="h-10 shrink-0 rounded-[10px]"
                    disabled={generatingToken}
                    onclick={() => generateRegistrationToken?.()}
                  >
                    {#if generatingToken}<LoaderCircle class="animate-spin" />{label(
                        'generatingRegistrationToken',
                        'Generating…',
                      )}{:else}<KeyRound />{tokenExpired
                        ? label('regenerateRegistrationToken', 'Generate a replacement')
                        : label('generateRegistrationToken', 'Generate token')}{/if}
                  </Button>
                {/if}
              </div>
            </div>

            <div
              class="flex min-h-[58px] items-center gap-3 rounded-[10px] border border-border bg-muted/55 px-3 py-2.5 sm:col-span-2"
            >
              <span class="min-w-0 flex-1 text-[13px] font-medium"
                >{label('allowApt', 'Allow apt package installation')}</span
              >
              <button
                type="button"
                role="switch"
                aria-checked={installAllowApt}
                aria-label={label('allowApt', 'Allow apt package installation')}
                class:apt-on={installAllowApt}
                class="apt-switch"
                onclick={() => (installAllowApt = !installAllowApt)}><span></span></button
              >
            </div>

            <div class="tunnel-mode-panel sm:col-span-2">
              {#key installTunnelMode}
                <div in:fly={{ y: 4, duration: 180 }} out:fade={{ duration: 100 }}>
                  {#if installTunnelMode === 'managed'}
                    <div class="managed-flow">
                      <div
                        class="progress-rail"
                        aria-label={label('connectionProgress', 'Cloudflare setup')}
                      >
                        <div class:done={cloudflareStage > 0} class:active={cloudflareStage === 0}>
                          <span>{cloudflareStage > 0 ? '✓' : '1'}</span>
                          <strong>{label('managedTunnelSetupTitle', 'Setup')}</strong>
                        </div>
                        <div class:done={cloudflareStage > 1} class:active={cloudflareStage === 1}>
                          <span>{cloudflareStage > 1 ? '✓' : '2'}</span>
                          <strong>OAuth</strong>
                        </div>
                        <div class:done={cloudflareStage > 2} class:active={cloudflareStage === 2}>
                          <span>{cloudflareStage > 2 ? '✓' : '3'}</span>
                          <strong>{label('cloudflareSelectZone', 'Zone')}</strong>
                        </div>
                        <div class:done={cloudflareStage > 3} class:active={cloudflareStage === 3}>
                          <span>{cloudflareStage > 3 ? '✓' : '4'}</span>
                          <strong>{label('createManagedTunnel', 'Tunnel')}</strong>
                        </div>
                      </div>
                      <div class="connection-state">
                        {#if managedStage === 'unconfigured'}
                          <div class="space-y-3">
                            <h3 class="text-[13px] font-semibold">
                              {label('managedTunnelSetupTitle', 'Configure managed tunnels')}
                            </h3>
                            <div
                              class="rounded-lg border border-border bg-background px-3 py-2 font-mono text-[11px] text-foreground shadow-sm"
                            >
                              {managedTunnelSetupCommand}
                            </div>
                            <div class="flex flex-wrap items-center gap-2">
                              <a
                                href={cloudflareApiTokenGuideUrl}
                                target="_blank"
                                rel="noreferrer"
                                class="inline-flex items-center gap-1 text-xs font-medium text-primary hover:underline"
                                >{label(
                                  'openCloudflareApiTokenGuide',
                                  'Cloudflare API token guide',
                                )}<ExternalLink class="size-3" /></a
                              >
                              <Button
                                variant="outline"
                                class="h-11 rounded-[10px] text-xs"
                                onclick={() =>
                                  copy(
                                    managedTunnelSetupCommand,
                                    label(
                                      'managedTunnelSetupCommandCopied',
                                      'Setup command copied',
                                    ),
                                  )}
                                ><Copy />{label(
                                  'copyManagedTunnelSetupCommand',
                                  'Copy setup command',
                                )}</Button
                              >
                            </div>
                          </div>
                        {:else if managedStage === 'needs_connect'}
                          <div class="step-line">
                            <div class="step-title">
                              <span class="step-index">1</span>{label(
                                'connectCloudflare',
                                'Connect Cloudflare',
                              )}
                            </div>
                            <Button
                              class="h-11 shrink-0 rounded-[10px] shadow-[0_1px_1px_oklch(20%_.04_255_/_0.18),0_5px_12px_oklch(40%_.12_255_/_0.16)]"
                              disabled={connectingCloudflare}
                              onclick={() => connectCloudflare?.()}
                              >{#if connectingCloudflare}<LoaderCircle
                                  class="animate-spin"
                                />{label('connectingCloudflare', 'Connecting…')}{:else}<Cloud
                                />{label('connectCloudflare', 'Connect Cloudflare')}{/if}</Button
                            >
                          </div>
                        {:else if managedStage === 'needs_zone'}
                          <div class="space-y-3">
                            <div class="step-title">
                              <span class="step-index done"><Check class="size-3" /></span>{label(
                                'cloudflareSelectZone',
                                'Select a zone',
                              )}
                            </div>
                            {#if loadingCloudflareZones}<div
                                class="flex items-center gap-2 text-xs text-muted-foreground"
                              >
                                <LoaderCircle class="size-4 animate-spin" />{label(
                                  'cloudflareLoadingZones',
                                  'Loading available zones…',
                                )}
                              </div>{:else}<Select.Root
                                type="single"
                                value={selectedCloudflareZone}
                                onValueChange={chooseZone}
                                disabled={selectingCloudflareZone || !cloudflareZones?.length}
                                ><Select.Trigger
                                  class="h-11 w-full rounded-[10px] border-border bg-background shadow-sm"
                                  >{selectedCloudflareZone
                                    ? cloudflareZones?.find(
                                        (zone) => zone.id === selectedCloudflareZone,
                                      )?.name
                                    : label(
                                        'cloudflareSelectZone',
                                        'Choose a zone',
                                      )}</Select.Trigger
                                ><Select.Content
                                  ><Select.Group
                                    >{#each cloudflareZones ?? [] as zone (zone.id)}<Select.Item
                                        value={zone.id}
                                        label={zone.name}>{zone.name}</Select.Item
                                      >{/each}</Select.Group
                                  ></Select.Content
                                ></Select.Root
                              >{/if}
                          </div>
                        {:else if managedStage === 'ready'}
                          <div class="step-line">
                            <div class="min-w-0">
                              <div class="step-title">
                                <span class="step-index done"><Check class="size-3" /></span>{label(
                                  'managedTunnelReady',
                                  'Stable tunnel ready',
                                )}
                              </div>
                              <p class="mt-1 truncate font-mono text-[11px] text-muted-foreground">
                                {managedProvision.backendId} · {managedProvision.publicUrl}
                              </p>
                            </div>
                            <Badge
                              class="rounded-md bg-emerald-500/12 text-emerald-700 dark:text-emerald-300"
                              ><Check />{label('ready', 'Ready')}</Badge
                            >
                          </div>
                        {:else}
                          <div class="space-y-3">
                            <div class="step-title">
                              <span class="step-index done"><Check class="size-3" /></span
                              >{cloudflareOAuth?.baseDomain}
                            </div>
                            <div class="flex flex-col gap-2 sm:flex-row sm:items-center">
                              <Select.Root
                                type="single"
                                value={selectedExistingTunnelId}
                                onValueChange={chooseExistingTunnel}
                                disabled={attachingTunnel ||
                                  loadingExistingTunnels ||
                                  reusableTunnels.length === 0}
                              >
                                <Select.Trigger
                                  class="h-11 w-full rounded-[10px] border-border bg-background shadow-sm sm:min-w-[18rem]"
                                >
                                  {#if loadingExistingTunnels}
                                    {label('loadingExistingTunnels', 'Loading tunnels…')}
                                  {:else if selectedExistingTunnelId}
                                    {tunnelOptionLabel(
                                      reusableTunnels.find(
                                        (tunnel) => tunnel.tunnelId === selectedExistingTunnelId,
                                      ) ?? {
                                        tunnelId: selectedExistingTunnelId,
                                        name: selectedExistingTunnelId,
                                        bound: false,
                                        deleted: false,
                                      },
                                    )}
                                  {:else}
                                    {label('selectExistingTunnel', 'Existing Tunnel')}
                                  {/if}
                                </Select.Trigger>
                                <Select.Content>
                                  <Select.Group>
                                    {#each reusableTunnels as tunnel (tunnel.tunnelId)}
                                      <Select.Item
                                        value={tunnel.tunnelId}
                                        label={tunnelOptionLabel(tunnel)}
                                        >{tunnelOptionLabel(tunnel)}</Select.Item
                                      >
                                    {/each}
                                  </Select.Group>
                                </Select.Content>
                              </Select.Root>
                              <Button
                                variant="outline"
                                size="icon"
                                class="size-11 shrink-0 rounded-[10px]"
                                disabled={loadingExistingTunnels || attachingTunnel}
                                aria-label={label('refreshExistingTunnels', 'Refresh')}
                                onclick={() => loadExistingTunnels?.()}
                              >
                                {#if loadingExistingTunnels}<LoaderCircle
                                    class="animate-spin"
                                  />{:else}<RefreshCw />{/if}
                              </Button>
                              <Button
                                class="h-11 shrink-0 rounded-[10px] shadow-[0_1px_1px_oklch(20%_.04_255_/_0.18),0_5px_12px_oklch(40%_.12_255_/_0.16)]"
                                disabled={provisioningTunnel || attachingTunnel}
                                onclick={() => ensureManagedProvision?.()}
                              >
                                {#if provisioningTunnel}<LoaderCircle class="animate-spin" />{label(
                                    'provisioningTunnel',
                                    'Creating tunnel…',
                                  )}{:else if attachingTunnel}<LoaderCircle
                                    class="animate-spin"
                                  />{label('attachingTunnel', 'Attaching tunnel…')}{:else}<Zap
                                  />{label('createManagedTunnel', 'Create new Tunnel')}{/if}
                              </Button>
                            </div>
                          </div>
                        {/if}
                      </div>
                    </div>
                  {:else}
                    <div class="quick-tunnel-marker">
                      <Cloud />
                      <span>{label('temporary', 'Temporary')} · trycloudflare.com</span>
                    </div>
                  {/if}
                </div>
              {/key}
            </div>
          </div>
        </Card.Content>
      </Card.Root>

      <Card.Root
        class="installer-card sticky top-24 gap-0 rounded-2xl border-white/10 bg-[oklch(17%_.012_250)] py-0 text-[oklch(92%_.006_250)] shadow-[0_2px_6px_oklch(8%_.01_250_/_0.28),0_20px_46px_oklch(8%_.01_250_/_0.24)] max-lg:static"
      >
        <Card.Header
          class="installer-card-header h-14 min-h-0 grid-cols-[minmax(0,1fr)_auto] grid-rows-1 items-center gap-2 border-white/8 px-4 py-0"
        >
          <div class="flex min-w-0 items-center gap-2.5">
            <span class="terminal-mark" aria-hidden="true"><Terminal /></span>
            <Card.Title
              class="truncate text-[15px] leading-none tracking-[-0.01em] whitespace-nowrap text-[oklch(95%_.005_250)]"
              >{label('installer', 'Installer command')}</Card.Title
            >
          </div>
          <Tooltip.Root delayDuration={200}>
            <Tooltip.Trigger>
              {#snippet child({ props })}
                <Button
                  {...props}
                  variant="ghost"
                  size="icon"
                  class="command-copy-button size-11 shrink-0 rounded-full"
                  disabled={!commandReady || provisioningTunnel}
                  aria-label={commandCopied
                    ? label('copied', 'Command copied')
                    : label('copy', 'Copy command')}
                  onclick={() => void copyCommand()}
                >
                  {#if commandCopied}<Check />{:else}<Copy />{/if}
                </Button>
              {/snippet}
            </Tooltip.Trigger>
            <Tooltip.Content sideOffset={8}>
              {commandCopied ? label('copied', 'Command copied') : label('copy', 'Copy command')}
            </Tooltip.Content>
          </Tooltip.Root>
        </Card.Header>
        <Card.Content class="command-content px-4 pt-2 pb-3.5">
          {#if waitingForTunnel}
            <div class="command-pending-surface" aria-disabled="true" aria-hidden="true">
              <Terminal />
            </div>
          {:else}
            <pre class="command-surface" aria-live="polite"><code>{installCommand.trimStart()}</code
              ></pre>
          {/if}
        </Card.Content>
      </Card.Root>
    </div>
  </section>
</Tooltip.Provider>

<style>
  :global(.tunnel-segment) {
    position: relative;
    isolation: isolate;
    border: 1px solid color-mix(in oklch, var(--border) 80%, transparent);
    border-radius: 0.875rem;
    background: var(--surface-soft);
    box-shadow: inset 0 1px 0 color-mix(in oklch, var(--surface) 72%, transparent);
  }
  :global(.tunnel-option) {
    position: relative;
    z-index: 1;
    min-width: 0;
    justify-content: flex-start;
    gap: clamp(0.3125rem, 1.6vw, 0.5rem);
    padding-inline: clamp(0.5rem, 2.1vw, 0.75rem);
    color: var(--muted-foreground);
    box-shadow: none;
    transition:
      background-color 160ms cubic-bezier(0.23, 1, 0.32, 1),
      color 160ms cubic-bezier(0.23, 1, 0.32, 1),
      box-shadow 160ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  :global(.tunnel-option) :global(svg) {
    width: 0.875rem;
    height: 0.875rem;
    flex: 0 0 auto;
  }
  :global(.tunnel-option[data-state='active']) {
    color: var(--foreground);
    background: transparent;
    box-shadow: none;
  }
  .tunnel-option-label {
    min-width: 0;
    flex: 1 1 auto;
    overflow: hidden;
    font-size: clamp(0.625rem, 2.85vw, 0.75rem);
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  :global(.tunnel-selection-indicator) {
    position: absolute;
    z-index: 0;
    top: 0.25rem;
    bottom: 0.25rem;
    left: 0.25rem;
    width: calc((100% - 0.75rem) / 2);
    border: 1px solid color-mix(in oklch, var(--border) 76%, transparent);
    border-radius: 0.625rem;
    background: var(--card);
    box-shadow:
      0 1px 2px color-mix(in oklch, var(--foreground) 8%, transparent),
      0 4px 10px color-mix(in oklch, var(--foreground) 5%, transparent);
    transform: translateX(0);
    transition: transform 180ms cubic-bezier(0.23, 1, 0.32, 1);
    pointer-events: none;
    will-change: transform;
  }
  :global(.tunnel-selection-indicator.quick) {
    transform: translateX(calc(100% + 0.25rem));
  }
  .mode-check {
    width: 1.125rem;
    height: 1.125rem;
    display: grid;
    flex: 0 0 1.125rem;
    place-items: center;
    margin-left: auto;
    border-radius: 999px;
    color: var(--primary-foreground);
    background: var(--primary);
    opacity: 0;
    transform: scale(0.82);
    transition:
      opacity 160ms cubic-bezier(0.23, 1, 0.32, 1),
      transform 160ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  :global(.tunnel-option[data-state='active']) .mode-check {
    opacity: 1;
    transform: scale(1);
  }
  .mode-check :global(svg) {
    width: 0.6875rem;
    height: 0.6875rem;
  }
  .field-label {
    color: var(--muted-foreground);
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 0.08em;
    line-height: 1.3;
    text-transform: uppercase;
  }
  .apt-switch {
    position: relative;
    width: 44px;
    height: 44px;
    flex: 0 0 auto;
    border: 0;
    border-radius: 12px;
    background: transparent;
    cursor: pointer;
  }
  .apt-switch::before {
    position: absolute;
    inset: 8px 0;
    border-radius: 14px;
    background: var(--border);
    content: '';
    transition: background 140ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  .apt-switch span {
    position: absolute;
    z-index: 1;
    top: 11px;
    left: 3px;
    width: 22px;
    height: 22px;
    border-radius: 999px;
    background: var(--card);
    box-shadow: 0 1px 3px oklch(20% 0.01 250 / 0.25);
    transition: transform 200ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  .apt-switch.apt-on::before {
    background: var(--primary);
  }
  .apt-switch.apt-on span {
    transform: translateX(16px);
  }
  .managed-flow {
    display: grid;
    gap: 0.75rem;
  }
  .tunnel-mode-panel {
    min-width: 0;
  }
  .progress-rail {
    display: grid;
    grid-template-columns: repeat(4, minmax(0, 1fr));
    overflow: hidden;
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    background: color-mix(in oklch, var(--muted) 62%, transparent);
  }
  .progress-rail > div {
    position: relative;
    min-width: 0;
    min-height: 3.25rem;
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.625rem;
    color: var(--muted-foreground);
  }
  .progress-rail > div + div {
    border-left: 1px solid var(--border);
  }
  .progress-rail span {
    width: 1.375rem;
    height: 1.375rem;
    display: grid;
    place-items: center;
    flex: 0 0 auto;
    border: 1px solid var(--border);
    border-radius: 50%;
    background: var(--card);
    font: 600 0.625rem/1 var(--font-mono);
  }
  .progress-rail strong {
    overflow: hidden;
    font-size: 0.6875rem;
    font-weight: 600;
    letter-spacing: 0.01em;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .progress-rail .active {
    color: var(--foreground);
    background: var(--card);
  }
  .progress-rail .active span {
    color: var(--primary-foreground);
    border-color: var(--primary);
    background: var(--primary);
    box-shadow: 0 0 0 3px color-mix(in oklch, var(--primary) 12%, transparent);
  }
  .progress-rail .done span {
    color: var(--success);
    border-color: color-mix(in oklch, var(--success) 32%, var(--border));
    background: var(--success-soft);
  }
  .connection-state {
    padding: 0.875rem;
    border: 1px solid var(--border);
    border-radius: 0.75rem;
    background: color-mix(in oklch, var(--muted) 35%, transparent);
  }
  .step-line {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
  }
  .step-title {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 13px;
    font-weight: 600;
  }
  .quick-tunnel-marker {
    min-height: 2.75rem;
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.625rem 0.75rem;
    border: 1px solid color-mix(in oklch, var(--warning) 26%, var(--border));
    border-radius: 0.625rem;
    color: color-mix(in oklch, var(--warning) 84%, var(--foreground));
    background: color-mix(in oklch, var(--warning-soft) 70%, transparent);
    font-size: 0.75rem;
    font-weight: 550;
  }
  .quick-tunnel-marker :global(svg) {
    width: 0.875rem;
    height: 0.875rem;
    flex: 0 0 auto;
  }
  .step-index {
    display: grid;
    place-items: center;
    width: 21px;
    height: 21px;
    flex: 0 0 auto;
    border-radius: 50%;
    background: var(--foreground);
    color: var(--background);
    font:
      600 10px/1 ui-monospace,
      monospace;
  }
  .step-index.done {
    background: oklch(54% 0.13 151);
    color: white;
  }
  :global(.installer-card) {
    gap: 0;
    overflow: hidden;
    background: oklch(17% 0.012 250);
    color: oklch(92% 0.006 250);
  }
  :global(.installer-card-header) {
    border-bottom: 1px solid oklch(100% 0 0 / 8%);
    background: transparent;
  }
  :global(.command-content) {
    padding-top: 0.5rem;
  }
  .terminal-mark {
    width: 1.875rem;
    height: 1.875rem;
    display: grid;
    place-items: center;
    flex: 0 0 auto;
    border: 1px solid oklch(100% 0 0 / 10%);
    border-radius: 0.5625rem;
    color: oklch(78% 0.008 250);
    background: oklch(100% 0 0 / 6%);
  }
  .terminal-mark :global(svg) {
    width: 0.9375rem;
    height: 0.9375rem;
  }
  .command-pending-surface {
    min-height: 3.5rem;
    display: grid;
    place-items: center;
    color: oklch(64% 0.008 250);
    opacity: 0.72;
  }
  .command-pending-surface :global(svg) {
    width: 1.125rem;
    height: 1.125rem;
  }
  .command-surface {
    display: block;
    min-height: 0;
    max-height: 20rem;
    margin: 0;
    overflow-x: auto;
    overflow-y: auto;
    padding: 0;
    color: oklch(90% 0.015 250);
    font-family: var(--font-mono);
    font-size: 0.75rem;
    line-height: 1.55;
    overflow-wrap: normal;
    word-break: normal;
    white-space: pre;
  }
  :global(.command-copy-button) {
    border-color: transparent;
    color: oklch(88% 0.006 250);
    background: transparent;
    box-shadow: none;
  }
  :global(.command-copy-button:hover) {
    color: oklch(100% 0 0);
    background: oklch(100% 0 0 / 10%);
  }
  .animate-in {
    animation: composer-in 200ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  @keyframes composer-in {
    from {
      opacity: 0;
      transform: translateY(3px);
    }
  }
  .token-row {
    border-color: var(--border);
    background: color-mix(in oklch, var(--muted) 55%, transparent);
    transition:
      border-color 160ms cubic-bezier(0.23, 1, 0.32, 1),
      background 160ms cubic-bezier(0.23, 1, 0.32, 1);
  }
  .token-row-active {
    border-color: color-mix(in oklch, var(--success) 34%, var(--border));
    background: color-mix(in oklch, var(--success-soft) 55%, transparent);
  }
  .token-row-expired {
    border-color: color-mix(in oklch, var(--warning) 32%, var(--border));
    background: color-mix(in oklch, var(--warning-soft) 55%, transparent);
  }
  .token-mark {
    width: 2rem;
    height: 2rem;
    display: grid;
    flex: 0 0 auto;
    place-items: center;
    border: 1px solid var(--border);
    border-radius: 0.5625rem;
    color: var(--muted-foreground);
    background: var(--card);
  }
  .token-mark :global(svg) {
    width: 0.9375rem;
    height: 0.9375rem;
  }
  .token-mark-active {
    color: var(--success);
    border-color: color-mix(in oklch, var(--success) 32%, var(--border));
    background: var(--success-soft);
  }
  .token-mark-expired {
    color: color-mix(in oklch, var(--warning) 84%, var(--foreground));
    border-color: color-mix(in oklch, var(--warning) 30%, var(--border));
    background: color-mix(in oklch, var(--warning-soft) 70%, transparent);
  }
  .token-headline {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .token-title {
    font-size: 0.8125rem;
    font-weight: 600;
  }
  .token-sub {
    display: flex;
    align-items: center;
    gap: 0.3125rem;
    margin-top: 0.125rem;
    color: var(--muted-foreground);
    font-size: 0.75rem;
    line-height: 1.4;
  }
  .token-sub :global(svg) {
    flex: 0 0 auto;
  }
  .token-sub-idle {
    margin-top: 0;
  }
  .token-sub-low {
    color: color-mix(in oklch, var(--warning) 82%, var(--foreground));
  }
  .token-clock {
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    font-weight: 600;
    letter-spacing: 0.01em;
  }
  @media (max-width: 640px) {
    .progress-rail {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .progress-rail > div:nth-child(3) {
      border-left: 0;
      border-top: 1px solid var(--border);
    }
    .progress-rail > div:nth-child(4) {
      border-top: 1px solid var(--border);
    }
    .step-line {
      align-items: flex-start;
      flex-direction: column;
    }
    .step-line :global(button) {
      width: 100%;
    }
    .apt-switch {
      margin-left: 8px;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    :global(.tunnel-selection-indicator),
    .mode-check {
      transition: none;
    }
    .tunnel-mode-panel > div {
      animation: none !important;
    }
  }
</style>
