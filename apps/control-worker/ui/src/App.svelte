<script lang="ts">
  import { onMount } from 'svelte';
  import { Toaster, toast } from 'svelte-sonner';
  import { Button } from '$lib/components/ui/button/index.js';
  import { Input } from '$lib/components/ui/input/index.js';
  import LanguagesIcon from '@lucide/svelte/icons/languages';
  import LockKeyholeIcon from '@lucide/svelte/icons/lock-keyhole';
  import LogOutIcon from '@lucide/svelte/icons/log-out';
  import MoonIcon from '@lucide/svelte/icons/moon';
  import ServerIcon from '@lucide/svelte/icons/server';
  import SunIcon from '@lucide/svelte/icons/sun';
  import FleetView from './FleetView.svelte';
  import InstallComposer from './InstallComposer.svelte';
  import { m } from './paraglide/messages.js';
  import { getLocale, setLocale } from './paraglide/runtime.js';

  type Locale = 'zh-CN' | 'en';
  type Theme = 'light' | 'dark';
  type TunnelMode = 'managed' | 'quick';
  type Filter = 'all' | 'online' | 'offline' | 'pending';
  type AuthState = 'checking' | 'anonymous' | 'authenticated';

  class AuthenticationRequiredError extends Error {}

  const origin = window.location.origin;
  const repositoryUrl = 'https://github.com/Ykmmj/vacps.git';
  const installDraftStorageKey = 'vacps-install-draft';
  const cloudflareApiTokenGuideUrl =
    'https://developers.cloudflare.com/fundamentals/oauth/create-an-oauth-client/';
  const managedTunnelSetupCommand = 'pnpm configure:managed-tunnels';

  let dashboard = $state<any>();
  let loading = $state(true);
  let authState = $state<AuthState>('checking');
  let loginPassword = $state('');
  let authenticating = $state(false);
  let activeView = $state<'fleet' | 'install'>('fleet');
  let filter = $state<Filter>('all');
  let actingId = $state<string | undefined>();
  let locale = $state<Locale>(getLocale() as Locale);
  let theme = $state<Theme>('light');
  let installBackendName = $state('');
  let installTags = $state('production,full');
  let installRedisUrl = $state('');
  let registrationToken = $state<
    { token: string; expiresAt: string; controlPlanePublicKey: string } | undefined
  >();
  let generatingToken = $state(false);
  let now = $state(Date.now());
  let installAllowApt = $state(false);
  /** node = apps/vacps; native = vacps-native static binary. */
  let installRuntime = $state<'node' | 'native'>('node');
  let installNativeVersion = $state('0.1.0');
  let installTunnelMode = $state<TunnelMode>('managed');
  let managedProvision = $state<any>();
  let provisioningTunnel = $state(false);
  let attachingTunnel = $state(false);
  let existingTunnels = $state<
    Array<{
      tunnelId: string;
      name: string;
      backendId?: string;
      hostname?: string;
      publicUrl?: string;
      bound: boolean;
      boundBackendId?: string;
      deleted: boolean;
    }>
  >([]);
  let selectedExistingTunnelId = $state('');
  let loadingExistingTunnels = $state(false);
  let cloudflareOAuth = $state<any>();
  let cloudflareZones = $state<Array<{ id: string; name: string }> | undefined>();
  let selectedCloudflareZone = $state('');
  let connectingCloudflare = $state(false);
  let loadingCloudflareZones = $state(false);
  let selectingCloudflareZone = $state(false);
  let dashboardRequestInFlight = $state(false);
  let cleaningTestHistory = $state(false);

  let text = $derived.by(() => {
    locale;
    return {
      nodes: m.nodes(),
      install: m.install(),
      refresh: m.refresh(),
      light: m.light(),
      dark: m.dark(),
      language: m.language(),
      password: m.password(),
      signIn: m.signIn(),
      signingIn: m.signingIn(),
      logout: m.logout(),
      invalidCredentials: m.invalidCredentials(),
      sessionExpired: m.sessionExpired(),
      authUnavailable: m.authUnavailable(),
      checkingSession: m.checkingSession(),
      pending: m.pending(),
      approved: m.approved(),
      rejected: m.rejected(),
      online: m.online(),
      offline: m.offline(),
      ip: m.ip(),
      location: m.location(),
      cpu: m.cpu(),
      memory: m.memory(),
      queue: m.queue(),
      stable: m.stable(),
      temporary: m.temporary(),
      approve: m.approve(),
      reject: m.reject(),
      test: m.test(),
      remove: m.remove(),
      all: m.all(),
      noNodes: m.noNodes(),
      noNodesHint: m.noNodesHint(),
      installer: m.installer(),
      tunnelMode: m.tunnelMode(),
      managedTunnel: m.managedTunnel(),
      quickTunnel: m.quickTunnel(),
      provisioningTunnel: m.provisioningTunnel(),
      attachingTunnel: m.attachingTunnel(),
      createManagedTunnel: m.createManagedTunnel(),
      selectExistingTunnel: m.selectExistingTunnel(),
      loadingExistingTunnels: m.loadingExistingTunnels(),
      refreshExistingTunnels: m.refreshExistingTunnels(),
      tunnelBound: m.tunnelBound(),
      tunnelAvailable: m.tunnelAvailable(),
      managedTunnelReady: m.managedTunnelReady(),
      installCommandPending: m.installCommandPending(),
      managedTunnelNeedsSetup: m.managedTunnelNeedsSetup(),
      managedTunnelSetupTitle: m.managedTunnelSetupTitle(),
      managedTunnelSetupDescription: m.managedTunnelSetupDescription(),
      openCloudflareApiTokenGuide: m.openCloudflareApiTokenGuide(),
      copyManagedTunnelSetupCommand: m.copyManagedTunnelSetupCommand(),
      managedTunnelSetupCommandCopied: m.managedTunnelSetupCommandCopied(),
      cloudflareSelectZone: m.cloudflareSelectZone(),
      cloudflareLoadingZones: m.cloudflareLoadingZones(),
      cloudflareSelectZoneHint: m.cloudflareSelectZoneHint(),
      cloudflareZoneReady: m.cloudflareZoneReady(),
      connectCloudflare: m.connectCloudflare(),
      connectingCloudflare: m.connectingCloudflare(),
      cloudflareConnectedHint: m.cloudflareConnectedHint(),
      quickTunnelNotice: m.quickTunnelNotice(),
      nodeName: m.nodeName(),
      tags: m.tags(),
      redisUrl: m.redisUrl(),
      redisUrlHint: m.redisUrlHint(),
      agentRuntime: 'Agent runtime',
      runtimeNode: 'Node (apps/vacps)',
      runtimeNative: 'Native (vacps-native)',
      nativeVersion: 'Native version',
      registrationToken: m.registrationToken(),
      registrationTokenHint: m.registrationTokenHint(),
      generateRegistrationToken: m.generateRegistrationToken(),
      generatingRegistrationToken: m.generatingRegistrationToken(),
      regenerateRegistrationToken: m.regenerateRegistrationToken(),
      registrationTokenReadyStatus: m.registrationTokenReadyStatus(),
      registrationTokenOneTime: m.registrationTokenOneTime(),
      registrationTokenExpiresIn: m.registrationTokenExpiresIn(),
      registrationTokenExpired: m.registrationTokenExpired(),
      registrationTokenReady: m.registrationTokenReady(),
      registrationTokenFailed: m.registrationTokenFailed(),
      installTokenPending: m.installTokenPending(),
      allowApt: m.allowApt(),
      allowAptHint: m.allowAptHint(),
      copy: m.copy(),
      copied: m.copied(),
      syncFailed: m.syncFailed(),
      approveOk: m.approveOk(),
      approveFailed: m.approveFailed(),
      rejectOk: m.rejectOk(),
      rejectFailed: m.rejectFailed(),
      testOk: m.testOk(),
      testFailed: m.testFailed(),
      removeOk: m.removeOk(),
      removeFailed: m.removeFailed(),
      confirmRemove: m.confirmRemove(),
      rejectPrompt: m.rejectPrompt(),
      runOnVps: m.runOnVps(),
      noTask: m.noTask(),
      mcpCopied: m.mcpCopied(),
      control: m.control(),
      footer: m.footer(),
      clipboardUnavailable: m.clipboardUnavailable(),
      cachedTelemetry: m.cachedTelemetry(),
      system: m.system(),
      download: m.download(),
      upload: m.upload(),
      unavailable: m.unavailable(),
      recentFailures: m.recentFailures(),
      recentFailuresHint: m.recentFailuresHint({ days: '{days}' }),
      noRecentFailures: m.noRecentFailures(),
      clearTestHistory: m.clearTestHistory({ count: '{count}' }),
      clearingTestHistory: m.clearingTestHistory(),
      confirmClearTestHistory: m.confirmClearTestHistory({ count: '{count}' }),
      clearTestHistoryOk: m.clearTestHistoryOk({ count: '{count}' }),
      clearTestHistoryNone: m.clearTestHistoryNone(),
      clearTestHistoryFailed: m.clearTestHistoryFailed(),
    };
  });
  const tokenExpiry = $derived(registrationToken ? Date.parse(registrationToken.expiresAt) : 0);
  const tokenActive = $derived(Boolean(registrationToken) && tokenExpiry > now);
  const tokenExpired = $derived(Boolean(registrationToken) && tokenExpiry <= now);
  const tokenMsRemaining = $derived(tokenActive ? Math.max(0, tokenExpiry - now) : 0);
  let installCommand = $derived(buildInstallCommand());

  $effect(() => {
    document.documentElement.dataset.theme = theme;
  });
  $effect(() => {
    document.documentElement.lang = locale;
  });
  $effect(() => {
    if (!registrationToken || tokenExpired) return;
    const timer = window.setInterval(() => {
      now = Date.now();
    }, 1000);
    return () => window.clearInterval(timer);
  });

  onMount(() => {
    restoreInstallDraft();
    const savedTheme = localStorage.getItem('vacps-theme');
    theme =
      savedTheme === 'dark' ||
      (savedTheme !== 'light' && matchMedia('(prefers-color-scheme: dark)').matches)
        ? 'dark'
        : 'light';
    void checkSession();
  });

  async function api<T>(path: string, init: RequestInit = {}): Promise<T> {
    const headers = new Headers(init.headers);
    headers.set('content-type', 'application/json');
    const response = await fetch(path, { ...init, headers, credentials: 'same-origin' });
    if (response.status === 204) return undefined as T;
    const body = (await response.json().catch(() => undefined)) as any;
    // Only the control-panel session uses authentication_required. Other 401s (or historical
    // Cloudflare OAuth 401s) must not force a logout and bounce the operator to the login screen.
    if (
      response.status === 401 &&
      (body?.error?.code === 'authentication_required' || body?.error?.code === undefined)
    ) {
      returnToLogin(true);
      throw new AuthenticationRequiredError();
    }
    if (!response.ok) throw new Error(body?.error?.message ?? `HTTP ${response.status}`);
    return body as T;
  }

  async function checkSession() {
    try {
      const response = await fetch('/api/auth/session', {
        credentials: 'same-origin',
        headers: { accept: 'application/json' },
      });
      if (response.ok) {
        await beginAuthenticatedSession();
        return;
      }
      if (response.status !== 401) notice(text.authUnavailable, 'error');
    } catch {
      notice(text.authUnavailable, 'error');
    }
    loading = false;
    authState = 'anonymous';
  }

  async function beginAuthenticatedSession() {
    authState = 'authenticated';
    loading = true;
    const cloudflareReturn = consumeCloudflareAuthorization();
    await Promise.all([refresh(), refreshCloudflare()]);
    // OAuth callback claims success, but status must still report a usable token.
    if (cloudflareReturn === 'connected' && !cloudflareOAuth?.connected) {
      notice(m.cloudflareAuthorizationFailed() + ' (status_not_connected)', 'error');
    }
  }

  function consumeCloudflareAuthorization(): string | undefined {
    const authorization = new URLSearchParams(location.search).get('cloudflare') ?? undefined;
    if (!authorization) return undefined;
    activeView = 'install';
    const url = new URL(location.href);
    url.searchParams.delete('cloudflare');
    history.replaceState({}, '', url);
    notice(
      authorization === 'connected'
        ? m.cloudflareAuthorizationComplete()
        : `${m.cloudflareAuthorizationFailed()} (${authorization})`,
      authorization === 'connected' ? 'success' : 'error',
    );
    return authorization;
  }

  async function login(event: SubmitEvent) {
    event.preventDefault();
    if (!loginPassword || authenticating) return;
    authenticating = true;
    try {
      const response = await fetch('/api/auth/login', {
        method: 'POST',
        credentials: 'same-origin',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ password: loginPassword }),
      });
      if (!response.ok) {
        notice(response.status === 401 ? text.invalidCredentials : text.authUnavailable, 'error');
        return;
      }
      loginPassword = '';
      await beginAuthenticatedSession();
    } catch {
      notice(text.authUnavailable, 'error');
    } finally {
      authenticating = false;
    }
  }

  async function logout() {
    try {
      await api('/api/auth/logout', { method: 'POST' });
      returnToLogin(false);
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
    }
  }

  function returnToLogin(announce: boolean) {
    dashboard = undefined;
    registrationToken = undefined;
    managedProvision = undefined;
    cloudflareOAuth = undefined;
    cloudflareZones = undefined;
    selectedCloudflareZone = '';
    loading = false;
    activeView = 'fleet';
    authState = 'anonymous';
    if (announce) notice(text.sessionExpired);
  }

  function isAuthenticationRequired(error: unknown): error is AuthenticationRequiredError {
    return error instanceof AuthenticationRequiredError;
  }

  function notice(message: string, tone: 'default' | 'error' | 'success' = 'default') {
    const options = { duration: tone === 'error' ? 6500 : 3500 };
    if (tone === 'error') toast.error(message, options);
    else if (tone === 'success') toast.success(message, options);
    else toast(message, options);
  }
  function messageOf(error: unknown) {
    return error instanceof Error ? error.message : String(error);
  }
  async function refresh(background = false) {
    if (dashboardRequestInFlight) return;
    dashboardRequestInFlight = true;
    if (!background) loading = true;
    try {
      dashboard = await api('/api/dashboard');
    } catch (error) {
      if (!background && !isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
    } finally {
      dashboardRequestInFlight = false;
      if (!background) loading = false;
    }
  }
  function handleVisibilityChange() {
    if (document.visibilityState === 'visible' && authState === 'authenticated') {
      void refresh(true);
    }
  }
  // Poll while authenticated (any tab) so a newly registered node shows up without a manual refresh.
  // Interval is fixed so dashboard updates do not re-enter this effect and thrash the API.
  $effect(() => {
    if (authState !== 'authenticated') return;
    const timer = window.setInterval(() => {
      if (document.visibilityState === 'visible') void refresh(true);
    }, 5_000);
    return () => window.clearInterval(timer);
  });
  async function refreshCloudflare() {
    try {
      cloudflareOAuth = await api('/api/cloudflare/oauth/status');
      cloudflareZones = undefined;
      selectedCloudflareZone = '';
      if (!cloudflareOAuth.connected) {
        // Expired or disconnected tokens fall back to the plain Connect Cloudflare step.
        existingTunnels = [];
        managedProvision = undefined;
        selectedExistingTunnelId = '';
        return;
      }
      if (cloudflareOAuth.connected && !cloudflareOAuth.zoneId) await loadCloudflareZones();
      if (cloudflareOAuth.connected && cloudflareOAuth.zoneId) await loadExistingTunnels();
      else existingTunnels = [];
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
    }
  }

  function markCloudflareDisconnected() {
    cloudflareOAuth = {
      configured: cloudflareOAuth?.configured ?? true,
      connected: false,
      accountId: cloudflareOAuth?.accountId,
    };
    existingTunnels = [];
    managedProvision = undefined;
    selectedExistingTunnelId = '';
  }

  function isCloudflareAuthorizationError(detail: string): boolean {
    return (
      detail.includes('Connect Cloudflare again') ||
      detail.includes('authorization expired') ||
      detail.includes('authorization data cannot be decrypted') ||
      detail.includes('invalid_grant') ||
      detail.includes('invalid_client')
    );
  }
  async function connectCloudflare() {
    connectingCloudflare = true;
    try {
      const { authorizationUrl } = await api<{ authorizationUrl: string }>(
        '/api/cloudflare/oauth/connect',
        { method: 'POST' },
      );
      saveInstallDraft();
      location.assign(authorizationUrl);
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
      connectingCloudflare = false;
    }
  }
  async function loadCloudflareZones() {
    loadingCloudflareZones = true;
    try {
      const zones = await api<Array<{ id: string; name: string }>>('/api/cloudflare/oauth/zones');
      cloudflareZones = zones;
      if (zones.length === 1 && zones[0]) await selectCloudflareZone(zones[0].id, false);
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
    } finally {
      loadingCloudflareZones = false;
    }
  }
  async function selectCloudflareZone(zoneId: string, announce = true) {
    selectingCloudflareZone = true;
    try {
      cloudflareOAuth = await api('/api/cloudflare/oauth/zone', {
        method: 'POST',
        body: JSON.stringify({ zoneId }),
      });
      await loadExistingTunnels();
      if (announce) notice(m.cloudflareZoneReady(), 'success');
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.syncFailed}${messageOf(error)}`, 'error');
    } finally {
      selectingCloudflareZone = false;
    }
  }
  async function approve(node: any) {
    actingId = node.registration.id;
    try {
      await api(`/api/registrations/${node.registration.id}/approve`, { method: 'POST' });
      notice(`${node.registration.name} ${text.approveOk}`, 'success');
      await refresh();
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.approveFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }
  async function reject(node: any) {
    const reason = prompt(text.rejectPrompt);
    if (reason === null) return;
    actingId = node.registration.id;
    try {
      await api(`/api/registrations/${node.registration.id}/reject`, {
        method: 'POST',
        body: JSON.stringify({ reason: reason.trim() || undefined }),
      });
      notice(`${node.registration.name} ${text.rejectOk}`);
      await refresh();
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.rejectFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }
  async function testBackend(node: any) {
    if (!node.backend) return;
    actingId = node.backend.id;
    try {
      await api(`/api/backends/${node.backend.id}/test`, { method: 'POST' });
      notice(`${node.registration.name} ${text.testOk}`, 'success');
      await refresh();
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.testFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }
  async function deleteBackend(node: any) {
    if (!confirm(text.confirmRemove)) return;
    const backendId = node.backend?.id ?? node.registration?.backendId;
    const registrationId = node.registration?.id;
    if (!backendId && !registrationId) return;
    actingId = backendId ?? registrationId;
    try {
      if (registrationId) await api(`/api/registrations/${registrationId}`, { method: 'DELETE' });
      else await api(`/api/backends/${backendId}`, { method: 'DELETE' });
      notice(`${node.registration?.name ?? backendId} ${text.removeOk}`);
      if (managedProvision?.backendId === backendId) managedProvision = undefined;
      await Promise.all([refresh(), loadExistingTunnels().catch(() => undefined)]);
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.removeFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }
  async function clearTestHistory() {
    if (cleaningTestHistory) return;
    cleaningTestHistory = true;
    try {
      const preview = await api<{
        matched_count: number;
        deletable_count: number;
      }>('/api/tasks/cleanup/preview', {
        method: 'POST',
        body: JSON.stringify({ filters: { test_only: true }, limit: 5000 }),
      });
      const count = preview.deletable_count ?? preview.matched_count ?? 0;
      if (count <= 0) {
        notice(text.clearTestHistoryNone);
        await refresh();
        return;
      }
      if (!confirm(text.confirmClearTestHistory.replace('{count}', String(count)))) return;
      const result = await api<{ deleted_count: number }>('/api/tasks/cleanup/run', {
        method: 'POST',
        body: JSON.stringify({
          filters: { test_only: true },
          mode: 'soft',
          reason: 'dashboard_clear_test_history',
          expected_matched_count: count,
          limit: 5000,
          idempotency_key: `dashboard-clear-test-${Date.now()}`,
        }),
      });
      notice(
        text.clearTestHistoryOk.replace('{count}', String(result.deleted_count ?? count)),
        'success',
      );
      await refresh();
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.clearTestHistoryFailed}${messageOf(error)}`, 'error');
    } finally {
      cleaningTestHistory = false;
    }
  }
  function toggleTheme() {
    theme = theme === 'light' ? 'dark' : 'light';
    localStorage.setItem('vacps-theme', theme);
  }
  function toggleLocale() {
    locale = locale === 'zh-CN' ? 'en' : 'zh-CN';
    setLocale(locale, { reload: false });
  }
  async function ensureManagedProvision() {
    if (managedProvision) return;
    provisioningTunnel = true;
    try {
      managedProvision = await api('/api/tunnels/provision', {
        method: 'POST',
        body: JSON.stringify({ name: installBackendName.trim() || undefined }),
      });
      selectedExistingTunnelId = managedProvision.tunnelId ?? '';
      notice(`${text.managedTunnelReady}: ${managedProvision.publicUrl}`, 'success');
      await loadExistingTunnels().catch(() => undefined);
    } catch (error) {
      if (isAuthenticationRequired(error)) return;
      const detail = messageOf(error);
      if (isCloudflareAuthorizationError(detail)) {
        markCloudflareDisconnected();
        return;
      }
      notice(
        detail.includes('Cloudflare OAuth is not configured')
          ? text.managedTunnelNeedsSetup
          : `${text.syncFailed}${detail}`,
        'error',
      );
    } finally {
      provisioningTunnel = false;
    }
  }
  async function loadExistingTunnels() {
    if (!cloudflareOAuth?.connected || !cloudflareOAuth?.zoneId) {
      existingTunnels = [];
      return;
    }
    loadingExistingTunnels = true;
    try {
      existingTunnels = await api('/api/tunnels');
    } catch (error) {
      existingTunnels = [];
      if (isAuthenticationRequired(error)) return;
      const detail = messageOf(error);
      if (isCloudflareAuthorizationError(detail)) {
        markCloudflareDisconnected();
        return;
      }
      notice(`${text.syncFailed}${detail}`, 'error');
    } finally {
      loadingExistingTunnels = false;
    }
  }
  async function attachExistingTunnel(tunnelId: string) {
    if (!tunnelId || attachingTunnel) return;
    selectedExistingTunnelId = tunnelId;
    attachingTunnel = true;
    try {
      const tunnel = existingTunnels.find((item) => item.tunnelId === tunnelId);
      managedProvision = await api('/api/tunnels/attach', {
        method: 'POST',
        body: JSON.stringify({
          tunnelId,
          ...(tunnel?.backendId ? { backendId: tunnel.backendId } : {}),
        }),
      });
      notice(`${text.managedTunnelReady}: ${managedProvision.publicUrl}`, 'success');
      await loadExistingTunnels().catch(() => undefined);
    } catch (error) {
      if (isAuthenticationRequired(error)) return;
      const detail = messageOf(error);
      if (isCloudflareAuthorizationError(detail)) {
        markCloudflareDisconnected();
        return;
      }
      notice(`${text.syncFailed}${detail}`, 'error');
    } finally {
      attachingTunnel = false;
    }
  }
  async function generateRegistrationToken() {
    if (generatingToken) return;
    generatingToken = true;
    try {
      registrationToken = await api<{
        token: string;
        expiresAt: string;
        controlPlanePublicKey: string;
      }>('/api/registration-tokens', { method: 'POST' });
      now = Date.now();
      notice(text.registrationTokenReady, 'success');
    } catch (error) {
      if (!isAuthenticationRequired(error))
        notice(`${text.registrationTokenFailed}${messageOf(error)}`, 'error');
    } finally {
      generatingToken = false;
    }
  }
  async function copyInstallCommand(): Promise<boolean> {
    if (installTunnelMode === 'managed' && !managedProvision) return false;
    if (!tokenActive) return false;
    return writeToClipboard(installCommand, text.copied);
  }
  async function copyToClipboard(value: string, success?: string) {
    await writeToClipboard(value, success);
  }
  async function writeToClipboard(value: string, success?: string): Promise<boolean> {
    try {
      await navigator.clipboard.writeText(value);
      notice(success ?? String(text.copied), 'success');
      return true;
    } catch {
      notice(text.clipboardUnavailable, 'error');
      return false;
    }
  }
  function shellQuote(value: string) {
    return `'${value.replaceAll("'", "'\\\\''")}'`;
  }
  function buildInstallCommand() {
    if (installTunnelMode === 'managed' && !managedProvision)
      return `# ${text.installCommandPending}`;
    if (!tokenActive || !registrationToken || !registrationToken.controlPlanePublicKey)
      return `# ${text.installTokenPending}`;
    const lines = [
      `curl -fsSL ${origin}/agent.sh | sudo bash -s -- install \\`,
      `  --runtime ${installRuntime} \\`,
      `  --control-plane-url ${shellQuote(origin)} \\`,
      `  --registration-token ${shellQuote(registrationToken.token)} \\`,
      `  --control-plane-public-key ${shellQuote(registrationToken.controlPlanePublicKey)}`,
    ];
    if (installRuntime === 'node') {
      lines[lines.length - 1] += ' \\';
      lines.push(
        `  --repo ${shellQuote(repositoryUrl)} \\`,
        `  --redis-url ${shellQuote(installRedisUrl || '<REDIS_URL>')}`,
      );
    } else {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --native-version ${shellQuote(installNativeVersion.trim() || '0.1.0')}`);
    }
    if (installTunnelMode === 'managed') {
      lines[lines.length - 1] += ' \\';
      lines.push(
        `  --backend-id ${shellQuote(managedProvision.backendId)} \\`,
        `  --public-url ${shellQuote(managedProvision.publicUrl)} \\`,
        `  --tunnel-token ${shellQuote(managedProvision.tunnelToken)}`,
      );
    }
    if (installTunnelMode === 'quick') {
      lines[lines.length - 1] += ' \\';
      lines.push('  --quick-tunnel');
    }
    if (installBackendName.trim()) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --backend-name ${shellQuote(installBackendName.trim())}`);
    }
    if (installTags.trim()) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --tags ${shellQuote(installTags.trim())}`);
    }
    if (installAllowApt) {
      lines[lines.length - 1] += ' \\';
      lines.push('  --allow-apt');
    }
    return lines.join('\n');
  }
  function saveInstallDraft() {
    sessionStorage.setItem(
      installDraftStorageKey,
      JSON.stringify({
        backendName: installBackendName,
        tags: installTags,
        redisUrl: installRedisUrl,
        allowApt: installAllowApt,
        runtime: installRuntime,
        nativeVersion: installNativeVersion,
        tunnelMode: installTunnelMode,
      }),
    );
  }
  function restoreInstallDraft() {
    try {
      const stored = sessionStorage.getItem(installDraftStorageKey);
      if (!stored) return;
      const draft = JSON.parse(stored);
      installBackendName = draft.backendName ?? '';
      installTags = draft.tags ?? installTags;
      installRedisUrl = draft.redisUrl ?? '';
      installAllowApt = draft.allowApt === true;
      installRuntime = draft.runtime === 'native' ? 'native' : 'node';
      installNativeVersion =
        typeof draft.nativeVersion === 'string' && draft.nativeVersion
          ? draft.nativeVersion
          : installNativeVersion;
      installTunnelMode = draft.tunnelMode === 'quick' ? 'quick' : 'managed';
    } catch {
    } finally {
      sessionStorage.removeItem(installDraftStorageKey);
    }
  }
</script>

<svelte:head><title>Vacps Control</title></svelte:head>
<svelte:document onvisibilitychange={handleVisibilityChange} />
<Toaster {theme} position="top-right" closeButton richColors />
<div
  class:dark={theme === 'dark'}
  class:authenticated-shell={authState === 'authenticated'}
  data-theme={theme}
  class="app-shell min-h-screen overflow-x-hidden bg-background text-foreground transition-colors duration-200"
>
  {#if authState === 'authenticated'}
    <header class="app-header sticky top-0 z-30 px-3 pt-3 sm:px-6 sm:pt-4">
      <div
        class="app-topbar mx-auto flex h-16 max-w-[1440px] items-center gap-2 rounded-2xl border px-2.5 pl-3 sm:gap-6 sm:px-3 sm:pl-4"
      >
        <button
          class="brand-button flex h-11 items-center gap-2.5 rounded-xl font-semibold tracking-[-0.01em]"
          onclick={() => (activeView = 'fleet')}
          aria-label="Vacps Control"
        >
          <span
            class="grid size-8 place-items-center rounded-[10px] bg-foreground text-card shadow-[inset_0_0_0_1px_oklch(100%_0_0_/_0.12)]"
            ><ServerIcon class="size-4" /></span
          >
          <span class="brand-label">Vacps</span>
        </button>
        <nav class="flex gap-1" aria-label="Primary navigation">
          <Button
            variant="ghost"
            class={`nav-button h-11 rounded-xl px-3 text-sm ${activeView === 'fleet' ? 'nav-active' : ''}`}
            aria-current={activeView === 'fleet' ? 'page' : undefined}
            onclick={() => {
              activeView = 'fleet';
              void refresh(true);
            }}>{text.nodes}</Button
          >
          <Button
            variant="ghost"
            class={`nav-button h-11 rounded-xl px-3 text-sm ${activeView === 'install' ? 'nav-active' : ''}`}
            aria-current={activeView === 'install' ? 'page' : undefined}
            onclick={() => (activeView = 'install')}>{text.install}</Button
          >
        </nav>
        <div class="ml-auto flex items-center gap-1">
          <Button
            variant="ghost"
            class="language-button h-11 rounded-xl px-2.5"
            aria-label={text.language}
            onclick={toggleLocale}
            ><LanguagesIcon /><span class="language-code">{locale === 'zh-CN' ? 'EN' : '中'}</span
            ></Button
          >
          <Button
            variant="ghost"
            size="icon"
            class="size-11 rounded-xl"
            aria-label={theme === 'dark' ? text.light : text.dark}
            onclick={toggleTheme}
            >{#if theme === 'dark'}<SunIcon />{:else}<MoonIcon />{/if}</Button
          >
          <Button
            variant="ghost"
            size="icon"
            class="size-11 rounded-xl"
            aria-label={text.logout}
            onclick={logout}><LogOutIcon /></Button
          >
        </div>
      </div>
    </header>
    <main
      class={`app-authenticated-main relative z-10 mx-auto w-[min(1392px,calc(100%-1.5rem))] sm:w-[min(1392px,calc(100%-3rem))] ${activeView === 'fleet' ? 'py-6 sm:py-8' : 'py-9 sm:py-14'}`}
    >
      {#if activeView === 'fleet'}
        <FleetView
          {text}
          {dashboard}
          {loading}
          bind:filter
          {actingId}
          {cleaningTestHistory}
          {approve}
          {reject}
          {testBackend}
          {deleteBackend}
          {clearTestHistory}
          {refresh}
          {copyToClipboard}
        />
      {:else}
        <InstallComposer
          {text}
          bind:installBackendName
          bind:installTags
          bind:installRedisUrl
          bind:installAllowApt
          bind:installRuntime
          bind:installNativeVersion
          bind:installTunnelMode
          {installCommand}
          {tokenActive}
          {tokenExpired}
          {tokenMsRemaining}
          {generatingToken}
          {generateRegistrationToken}
          {managedProvision}
          {provisioningTunnel}
          {attachingTunnel}
          {existingTunnels}
          bind:selectedExistingTunnelId
          {loadingExistingTunnels}
          {cloudflareOAuth}
          {cloudflareZones}
          bind:selectedCloudflareZone
          {connectingCloudflare}
          {loadingCloudflareZones}
          {selectingCloudflareZone}
          {managedTunnelSetupCommand}
          {cloudflareApiTokenGuideUrl}
          {connectCloudflare}
          {selectCloudflareZone}
          {ensureManagedProvision}
          {loadExistingTunnels}
          {attachExistingTunnel}
          {copyInstallCommand}
          {copyToClipboard}
        />
      {/if}
    </main>
    <footer
      class="app-footer relative z-10 mx-auto flex w-[min(1392px,calc(100%-1.5rem))] items-center gap-3 border-t border-border/70 py-5 text-[11px] text-muted-foreground sm:w-[min(1392px,calc(100%-3rem))]"
    >
      <span>{text.footer}</span><button
        onclick={() => copyToClipboard(`${origin}/mcp`, text.mcpCopied)}>MCP</button
      ><a href="https://github.com/Ykmmj/vacps" target="_blank" rel="noreferrer">GitHub</a>
    </footer>
  {:else}
    <main class="relative grid min-h-screen place-items-center px-4 py-8">
      <section
        class="relative w-full max-w-sm rounded-2xl border border-border/80 bg-card/90 p-6 shadow-elevated backdrop-blur-sm sm:p-7"
        aria-live="polite"
      >
        <div class="absolute top-3 right-3 flex items-center gap-1">
          <Button
            variant="ghost"
            size="icon"
            class="size-10 rounded-xl"
            aria-label={text.language}
            onclick={toggleLocale}><LanguagesIcon /></Button
          >
          <Button
            variant="ghost"
            size="icon"
            class="size-10 rounded-xl"
            aria-label={theme === 'dark' ? text.light : text.dark}
            onclick={toggleTheme}
            >{#if theme === 'dark'}<SunIcon />{:else}<MoonIcon />{/if}</Button
          >
        </div>
        <div class="flex items-center gap-3 pr-20">
          <span
            class="grid size-11 place-items-center rounded-[14px] bg-foreground text-card shadow-[inset_0_0_0_1px_oklch(100%_0_0_/_0.12)]"
            aria-hidden="true"><LockKeyholeIcon class="size-5" /></span
          >
          <div>
            <p class="text-[10px] font-semibold tracking-[0.1em] text-muted-foreground uppercase">
              {text.control}
            </p>
            <h1 class="mt-0.5 text-lg font-semibold tracking-[-0.02em]">Vacps</h1>
          </div>
        </div>
        {#if authState === 'checking'}
          <div class="mt-8 flex h-11 items-center gap-2 text-sm text-muted-foreground">
            <span class="size-2 animate-pulse rounded-full bg-primary" aria-hidden="true"></span>
            <span>{text.checkingSession}</span>
          </div>
        {:else}
          <form class="mt-8 grid gap-3" onsubmit={login}>
            <label class="grid gap-1.5">
              <span class="text-xs font-medium text-muted-foreground">{text.password}</span>
              <Input
                bind:value={loginPassword}
                type="password"
                autocomplete="current-password"
                class="h-12 rounded-xl border-border bg-background"
                required
              />
            </label>
            <Button class="mt-1 h-11 rounded-xl" type="submit" disabled={authenticating}
              >{authenticating ? text.signingIn : text.signIn}</Button
            >
          </form>
        {/if}
      </section>
    </main>
  {/if}
</div>
