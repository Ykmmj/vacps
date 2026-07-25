<script lang="ts">
  import { onMount } from 'svelte';

  type RegistrationStatus = 'pending' | 'approved' | 'rejected';
  type Locale = 'zh-CN' | 'en';
  type Theme = 'light' | 'dark';
  type TunnelMode = 'managed' | 'quick';

  interface Backend {
    id: string;
    name: string;
    baseUrl: string;
    tags: string[];
    enabled: boolean;
  }

  interface Registration {
    id: string;
    backendId: string;
    name: string;
    baseUrl: string;
    tags: string[];
    agentVersion: string;
    status: RegistrationStatus;
    requestedAt: string;
    updatedAt: string;
    decisionAt?: string;
    rejectionReason?: string;
    ip?: string;
    location?: string;
  }

  interface NodeStatus {
    health: { ok: boolean; uptimeSeconds: number; redis: { connected: boolean } };
    metrics?: {
      cpu: { usagePercent?: number; load1?: number; cores?: number };
      memory: { totalBytes: number; usedBytes: number };
      queue: { waiting: number; active: number; failed: number };
    };
  }

  interface Node {
    registration: Registration;
    backend?: Backend;
    status?: NodeStatus;
    online: boolean;
    checkedAt: string;
  }

  interface Dashboard {
    totals: {
      backends: number;
      enabledBackends: number;
      queued: number;
      active: number;
      schedules: number;
      pendingRegistrations: number;
    };
    nodes: Node[];
  }

  interface ManagedProvision {
    backendId: string;
    hostname: string;
    publicUrl: string;
    tunnelToken: string;
  }

  const copy = {
    'zh-CN': {
      nodes: '节点',
      install: '安装 Agent',
      refresh: '刷新',
      light: '浅色',
      dark: '深色',
      language: 'English',
      nodeCount: '个节点',
      pending: '待审批',
      approved: '已批准',
      rejected: '已拒绝',
      online: '在线',
      offline: '离线',
      ip: '公网 IP',
      location: '归属地',
      cpu: 'CPU',
      memory: '内存',
      checked: '刚刚检查',
      unavailable: '暂不可用',
      approve: '批准接入',
      reject: '拒绝',
      actions: '节点操作',
      test: '健康检查',
      remove: '移除节点',
      noNodes: '尚无节点',
      noNodesHint: '在 VPS 执行安装命令后，节点会自动出现在这里。',
      all: '全部',
      installer: '安装命令',
      tunnelMode: '接入方式',
      managedTunnel: '托管 Tunnel',
      managedTunnelDescription: '创建稳定域名、Tunnel 与 DNS；无需填写公开 URL。',
      quickTunnel: 'Quick Tunnel',
      quickTunnelDescription: '自动获取临时 trycloudflare.com 地址；适合演示和测试。',
      provisionTunnel: '创建稳定 Tunnel',
      provisioningTunnel: '正在创建 Tunnel…',
      managedTunnelReady: '稳定地址已准备好',
      managedTunnelNeedsSetup: '需要先为控制平面配置 Tunnel/DNS API Token。',
      quickTunnelNotice: '地址会在 cloudflared 重连后变化，Agent 会自动重新注册。',
      nodeName: '节点名称（可选）',
      tags: '标签（逗号分隔）',
      redisUrl: 'Redis URL（推荐 TLS）',
      redisUrlHint: '优先使用 rediss://；仅在 Redis 位于私有或受限网络时使用 redis://。',
      registrationSecret: '注册密钥',
      registrationSecretHint:
        '与 Cloudflare Worker 保存的密钥相同，仅用于 Agent 发起注册。网页不会保存它。',
      allowApt: '允许 Agent 安装 apt 软件包',
      allowAptHint: '包维护脚本可以 root 权限运行，仅在明确需要时启用。',
      copy: '复制命令',
      download: '下载脚本',
      copied: '安装命令已复制',
      syncOk: '已同步',
      syncFailed: '同步失败：',
      approveOk: '已批准并启用',
      approveFailed: '审批未完成：',
      rejectOk: '已拒绝',
      rejectFailed: '拒绝操作失败：',
      testOk: '健康检查通过',
      testFailed: '节点不可达：',
      removeOk: '已从控制平面移除',
      removeFailed: '移除失败：',
      confirmRemove: '确定移除该节点？此操作不会卸载 VPS 上的 Agent。',
      rejectPrompt: '可选：填写拒绝原因。Agent 下次注册会再次进入待审批状态。',
      autoId: '节点 ID 会由安装器自动随机生成。',
      setupTunnel: '选择接入方式后生成可直接运行的安装命令。',
      runOnVps: '在 VPS 执行',
      noTask: '任务通过 MCP 或计划工作流提交。',
      mcpCopied: 'MCP 地址已复制',
      backToNodes: '返回节点',
    },
    en: {
      nodes: 'Nodes',
      install: 'Install agent',
      refresh: 'Refresh',
      light: 'Light',
      dark: 'Dark',
      language: '中文',
      nodeCount: 'nodes',
      pending: 'Pending',
      approved: 'Approved',
      rejected: 'Rejected',
      online: 'Online',
      offline: 'Offline',
      ip: 'Public IP',
      location: 'Location',
      cpu: 'CPU',
      memory: 'Memory',
      checked: 'Just checked',
      unavailable: 'Unavailable',
      approve: 'Approve',
      reject: 'Reject',
      actions: 'Node actions',
      test: 'Health check',
      remove: 'Remove node',
      noNodes: 'No nodes yet',
      noNodesHint: 'Run the install command on a VPS and its card will appear here.',
      all: 'All',
      installer: 'Install command',
      tunnelMode: 'Connection mode',
      managedTunnel: 'Managed Tunnel',
      managedTunnelDescription:
        'Creates a stable hostname, Tunnel, and DNS. No public URL to enter.',
      quickTunnel: 'Quick Tunnel',
      quickTunnelDescription:
        'Gets a temporary trycloudflare.com URL automatically; best for demos and tests.',
      provisionTunnel: 'Create stable Tunnel',
      provisioningTunnel: 'Creating Tunnel…',
      managedTunnelReady: 'Stable endpoint is ready',
      managedTunnelNeedsSetup: 'Configure the Tunnel/DNS API token for this control plane first.',
      quickTunnelNotice:
        'The URL may change after cloudflared reconnects; the Agent re-registers automatically.',
      nodeName: 'Node name (optional)',
      tags: 'Tags (comma-separated)',
      redisUrl: 'Redis URL (TLS preferred)',
      redisUrlHint:
        'Prefer rediss://. Use redis:// only when Redis is on a private or restricted network.',
      registrationSecret: 'Registration secret',
      registrationSecretHint:
        'The same secret stored in the Cloudflare Worker. It only authorizes agent registration and is never saved by this page.',
      allowApt: 'Allow agent to install apt packages',
      allowAptHint: 'Package maintainer scripts may run as root. Enable only when needed.',
      copy: 'Copy command',
      download: 'Download script',
      copied: 'Install command copied',
      syncOk: 'Synchronized',
      syncFailed: 'Sync failed: ',
      approveOk: 'Approved and enabled',
      approveFailed: 'Approval failed: ',
      rejectOk: 'Rejected',
      rejectFailed: 'Reject failed: ',
      testOk: 'Health check passed',
      testFailed: 'Node unreachable: ',
      removeOk: 'Removed from the control plane',
      removeFailed: 'Removal failed: ',
      confirmRemove: 'Remove this node? This will not uninstall the agent on the VPS.',
      rejectPrompt:
        'Optional: add a rejection reason. The next registration request will return to pending.',
      autoId: 'The installer generates a random node ID automatically.',
      setupTunnel: 'Choose a connection mode to generate a ready-to-run command.',
      runOnVps: 'Run on the VPS',
      noTask: 'Tasks are submitted through MCP or schedules.',
      mcpCopied: 'MCP endpoint copied',
      backToNodes: 'Back to nodes',
    },
  } as const;

  const origin = window.location.origin;
  const repositoryUrl = 'https://github.com/Ykmmj/vps-agent-platform.git';

  let dashboard: Dashboard | undefined;
  let loading = true;
  let notice = '';
  let noticeTone: 'default' | 'error' | 'success' = 'default';
  let activeView: 'fleet' | 'install' = 'fleet';
  let filter: RegistrationStatus | 'all' = 'all';
  let actingId: string | undefined;
  let locale: Locale = 'zh-CN';
  let theme: Theme = 'light';

  let installBackendName = '';
  let installTags = 'production,full';
  let installRedisUrl = '';
  let installRegistrationSecret = '';
  let installAllowApt = false;
  let installTunnelMode: TunnelMode = 'managed';
  let managedProvision: ManagedProvision | undefined;
  let provisioningTunnel = false;

  $: text = copy[locale];
  $: visibleNodes = (dashboard?.nodes ?? []).filter(
    (node) => filter === 'all' || node.registration.status === filter,
  );
  $: installCommand = buildInstallCommand(
    installBackendName,
    installTags,
    installRedisUrl,
    installRegistrationSecret,
    installAllowApt,
    installTunnelMode,
    managedProvision,
  );

  onMount(() => {
    const savedLocale = localStorage.getItem('vps-agent-locale');
    if (savedLocale === 'zh-CN' || savedLocale === 'en') locale = savedLocale;
    const savedTheme = localStorage.getItem('vps-agent-theme');
    theme =
      savedTheme === 'dark' ||
      (savedTheme !== 'light' && matchMedia('(prefers-color-scheme: dark)').matches)
        ? 'dark'
        : 'light';
    applyTheme();
    void refresh();
  });

  async function api<T>(path: string, init: RequestInit = {}): Promise<T> {
    const headers = new Headers(init.headers);
    headers.set('content-type', 'application/json');
    const response = await fetch(path, { ...init, headers });
    if (response.status === 204) return undefined as T;
    const body = (await response.json().catch(() => undefined)) as
      T | { error?: { message?: string } } | undefined;
    if (!response.ok) {
      throw new Error(
        body && typeof body === 'object' && 'error' in body
          ? (body.error?.message ?? `HTTP ${response.status}`)
          : `HTTP ${response.status}`,
      );
    }
    return body as T;
  }

  function setNotice(message: string, tone: 'default' | 'error' | 'success' = 'default') {
    notice = message;
    noticeTone = tone;
  }

  async function refresh() {
    loading = true;
    try {
      dashboard = await api<Dashboard>('/api/dashboard');
      setNotice(text.syncOk, 'success');
    } catch (error) {
      setNotice(`${text.syncFailed}${messageOf(error)}`, 'error');
    } finally {
      loading = false;
    }
  }

  async function approve(node: Node) {
    actingId = node.registration.id;
    try {
      await api(`/api/registrations/${node.registration.id}/approve`, { method: 'POST' });
      setNotice(`${node.registration.name} ${text.approveOk}`, 'success');
      await refresh();
    } catch (error) {
      setNotice(`${text.approveFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function reject(node: Node) {
    const reason = window.prompt(text.rejectPrompt);
    if (reason === null) return;
    actingId = node.registration.id;
    try {
      await api(`/api/registrations/${node.registration.id}/reject`, {
        method: 'POST',
        body: JSON.stringify({ reason: reason.trim() || undefined }),
      });
      setNotice(`${node.registration.name} ${text.rejectOk}`);
      await refresh();
    } catch (error) {
      setNotice(`${text.rejectFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function testBackend(node: Node) {
    if (!node.backend) return;
    actingId = node.backend.id;
    try {
      await api(`/api/backends/${node.backend.id}/test`, { method: 'POST' });
      setNotice(`${node.registration.name} ${text.testOk}`, 'success');
      await refresh();
    } catch (error) {
      setNotice(`${text.testFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function deleteBackend(node: Node) {
    if (!node.backend || !window.confirm(text.confirmRemove)) return;
    actingId = node.backend.id;
    try {
      await api(`/api/backends/${node.backend.id}`, { method: 'DELETE' });
      setNotice(`${node.registration.name} ${text.removeOk}`);
      await refresh();
    } catch (error) {
      setNotice(`${text.removeFailed}${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  function toggleTheme() {
    theme = theme === 'light' ? 'dark' : 'light';
    localStorage.setItem('vps-agent-theme', theme);
    applyTheme();
  }

  function applyTheme() {
    document.documentElement.classList.toggle('dark', theme === 'dark');
  }

  function toggleLocale() {
    locale = locale === 'zh-CN' ? 'en' : 'zh-CN';
    localStorage.setItem('vps-agent-locale', locale);
    void refresh();
  }

  async function provisionManagedTunnel() {
    provisioningTunnel = true;
    try {
      managedProvision = await api<ManagedProvision>('/api/tunnels/provision', {
        method: 'POST',
        body: JSON.stringify({ name: installBackendName.trim() || undefined }),
      });
      setNotice(`${text.managedTunnelReady}: ${managedProvision.publicUrl}`, 'success');
    } catch (error) {
      const message = messageOf(error);
      setNotice(
        message.includes('Managed Tunnel is not configured')
          ? text.managedTunnelNeedsSetup
          : `${text.syncFailed}${message}`,
        'error',
      );
    } finally {
      provisioningTunnel = false;
    }
  }

  function buildInstallCommand(
    backendName: string,
    tags: string,
    redisUrl: string,
    registrationSecret: string,
    allowApt: boolean,
    tunnelMode: TunnelMode,
    provision: ManagedProvision | undefined,
  ) {
    if (tunnelMode === 'managed' && !provision) return `# ${text.provisionTunnel}`;
    const lines = [
      `curl -fsSL ${origin}/install-agent.sh | sudo bash -s -- \\`,
      `  --repo ${shellQuote(repositoryUrl)} \\`,
      `  --control-plane-url ${shellQuote(origin)} \\`,
      `  --backend-token ${shellQuote(registrationSecret || '<REGISTRATION_SECRET>')} \\`,
      `  --redis-url ${shellQuote(redisUrl || '<REDIS_URL>')}`,
    ];
    if (tunnelMode === 'managed' && provision) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --backend-id ${shellQuote(provision.backendId)} \\`);
      lines.push(`  --public-url ${shellQuote(provision.publicUrl)} \\`);
      lines.push(`  --tunnel-token ${shellQuote(provision.tunnelToken)}`);
    }
    if (tunnelMode === 'quick') {
      lines[lines.length - 1] += ' \\';
      lines.push('  --quick-tunnel');
    }
    if (backendName.trim()) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --backend-name ${shellQuote(backendName.trim())}`);
    }
    if (tags.trim()) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --tags ${shellQuote(tags.trim())}`);
    }
    if (allowApt) {
      lines[lines.length - 1] += ' \\';
      lines.push('  --allow-apt');
    }
    return lines.join('\n');
  }

  async function copyToClipboard(value: string, success: string) {
    try {
      await navigator.clipboard.writeText(value);
      setNotice(success, 'success');
    } catch {
      setNotice('Clipboard access is unavailable.', 'error');
    }
  }

  function shellQuote(value: string) {
    return `'${value.replaceAll("'", "'\\\\''")}'`;
  }

  function bytes(value: number | undefined) {
    if (!value) return '—';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    const exponent = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);
    return `${(value / 1024 ** exponent).toFixed(exponent > 2 ? 1 : 0)} ${units[exponent]}`;
  }

  function memoryPercent(node: Node) {
    const memory = node.status?.metrics?.memory;
    return memory && memory.totalBytes > 0
      ? Math.round((memory.usedBytes / memory.totalBytes) * 100)
      : undefined;
  }

  function cpuValue(node: Node) {
    const cpu = node.status?.metrics?.cpu;
    if (!cpu) return '—';
    if (typeof cpu.usagePercent === 'number') return `${cpu.usagePercent}%`;
    return typeof cpu.load1 === 'number' ? `load ${cpu.load1}` : '—';
  }

  function statusLabel(status: RegistrationStatus) {
    return status === 'pending'
      ? text.pending
      : status === 'approved'
        ? text.approved
        : text.rejected;
  }

  function displayDate(value: string) {
    return new Intl.DateTimeFormat(locale, { dateStyle: 'medium', timeStyle: 'short' }).format(
      new Date(value),
    );
  }

  function messageOf(error: unknown) {
    return error instanceof Error ? error.message : String(error);
  }
</script>

<svelte:head>
  <title>VPS Agent Control</title>
  <meta name="theme-color" content={theme === 'dark' ? '#09090b' : '#f5f5f7'} />
</svelte:head>

<div
  class="min-h-screen bg-[#f5f5f7] text-[#1d1d1f] transition-colors dark:bg-[#09090b] dark:text-zinc-100"
>
  <header
    class="sticky top-0 z-30 border-b border-black/[.055] bg-white/75 backdrop-blur-2xl dark:border-white/[.07] dark:bg-zinc-950/75"
  >
    <div
      class="mx-auto flex h-16 max-w-7xl items-center justify-between gap-3 px-4 sm:px-6 lg:px-8"
    >
      <button class="flex items-center gap-2.5 text-left" onclick={() => (activeView = 'fleet')}>
        <span
          class="grid h-9 w-9 place-items-center rounded-xl bg-[#0071e3] text-white shadow-lg shadow-blue-500/20"
        >
          <svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="2"
            class="h-5 w-5"
            aria-hidden="true"
            ><circle cx="6" cy="6" r="2" /><circle cx="18" cy="7" r="2" /><circle
              cx="12"
              cy="18"
              r="2"
            /><path d="m7.7 7.1 2.8 8.1M16.2 8.4l-2.7 7.2M8 6.3l8 .5" /></svg
          >
        </span>
        <span
          ><strong class="block text-sm tracking-[-.02em]">VPS Agent</strong><span
            class="block text-[10px] font-semibold tracking-[.13em] text-zinc-400">CONTROL</span
          ></span
        >
      </button>

      <nav
        class="hidden items-center rounded-full bg-zinc-100 p-1 dark:bg-zinc-800 sm:flex"
        aria-label="Primary navigation"
      >
        <button
          class:!bg-white={activeView === 'fleet'}
          class:shadow-sm={activeView === 'fleet'}
          class="rounded-full px-4 py-1.5 text-xs font-semibold text-zinc-600 transition dark:text-zinc-300 dark:!bg-zinc-700"
          onclick={() => (activeView = 'fleet')}>{text.nodes}</button
        >
        <button
          class:!bg-white={activeView === 'install'}
          class:shadow-sm={activeView === 'install'}
          class="rounded-full px-4 py-1.5 text-xs font-semibold text-zinc-600 transition dark:text-zinc-300 dark:!bg-zinc-700"
          onclick={() => (activeView = 'install')}>{text.install}</button
        >
      </nav>

      <div class="flex items-center gap-1.5">
        <button
          class="icon-button"
          title={text.language}
          aria-label={text.language}
          onclick={toggleLocale}
          ><svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="1.8"
            class="h-4 w-4"
            aria-hidden="true"
            ><circle cx="12" cy="12" r="9" /><path
              d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"
            /></svg
          ></button
        >
        <button
          class="icon-button"
          title={theme === 'dark' ? text.light : text.dark}
          aria-label={theme === 'dark' ? text.light : text.dark}
          onclick={toggleTheme}
          >{#if theme === 'dark'}<svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="1.8"
              class="h-4 w-4"
              aria-hidden="true"
              ><circle cx="12" cy="12" r="3.5" /><path
                d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"
              /></svg
            >{:else}<svg viewBox="0 0 24 24" fill="currentColor" class="h-4 w-4" aria-hidden="true"
              ><path
                d="M20.7 15.9A8.6 8.6 0 0 1 8.1 3.4a.7.7 0 0 0-.8.9 8.6 8.6 0 1 0 12.5 12.5.7.7 0 0 0 .9-.9Z"
              /></svg
            >{/if}</button
        >
        <button
          class="icon-button"
          title={text.refresh}
          aria-label={text.refresh}
          onclick={refresh}
          disabled={loading}
          ><svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="1.8"
            class:animate-spin={loading}
            class="spin-center h-4 w-4"
            aria-hidden="true"><path d="M20 12a8 8 0 1 1-2.35-5.65" /><path d="M20 4v5h-5" /></svg
          ></button
        >
      </div>
    </div>
  </header>

  <main class="mx-auto max-w-7xl px-4 py-6 sm:px-6 lg:px-8 lg:py-8">
    {#if activeView === 'fleet'}
      <div class="mb-6 flex flex-wrap items-center justify-between gap-3">
        <div class="flex items-center gap-3">
          <h1 class="text-2xl font-semibold tracking-[-.045em]">{text.nodes}</h1>
          <span
            class="rounded-full bg-zinc-200 px-2.5 py-1 text-xs font-semibold text-zinc-500 dark:bg-zinc-800 dark:text-zinc-400"
            >{dashboard?.nodes.length ?? 0} {text.nodeCount}</span
          >
        </div>
        <div class="flex items-center gap-2">
          <div class="hidden rounded-full bg-zinc-100 p-1 dark:bg-zinc-800 sm:flex">
            {#each ['all', 'pending', 'approved', 'rejected'] as option}
              <button
                class:!bg-white={filter === option}
                class:shadow-sm={filter === option}
                class="rounded-full px-3 py-1.5 text-xs font-semibold text-zinc-500 transition dark:text-zinc-400 dark:!bg-zinc-700"
                onclick={() => (filter = option as RegistrationStatus | 'all')}
                >{option === 'all' ? text.all : statusLabel(option as RegistrationStatus)}</button
              >
            {/each}
          </div>
          <button class="primary-button" onclick={() => (activeView = 'install')}
            ><svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="2"
              class="h-4 w-4"
              aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg
            ><span class="hidden sm:inline">{text.install}</span></button
          >
        </div>
      </div>

      {#if loading && !dashboard}
        <div class="grid min-h-80 place-items-center text-sm text-zinc-400">Loading nodes…</div>
      {:else if visibleNodes.length === 0}
        <section class="surface-card grid min-h-80 place-items-center p-8 text-center">
          <div>
            <span
              class="mx-auto grid h-12 w-12 place-items-center rounded-2xl bg-blue-50 text-[#0071e3] dark:bg-blue-500/10"
              ><svg
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="1.8"
                class="h-6 w-6"
                aria-hidden="true"><path d="M5 19V5m0 7h14m-5-5 5 5-5 5" /></svg
              ></span
            >
            <p class="mt-4 font-semibold">{text.noNodes}</p>
            <p class="mt-1 text-sm text-zinc-500 dark:text-zinc-400">{text.noNodesHint}</p>
          </div>
        </section>
      {:else}
        <section class="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
          {#each visibleNodes as node (node.registration.id)}
            <article class="surface-card group flex min-h-92 flex-col p-5 sm:p-6">
              <div class="flex items-start justify-between gap-3">
                <div class="min-w-0">
                  <div class="flex flex-wrap items-center gap-2">
                    <span
                      class:status-online={node.online}
                      class:status-offline={!node.online}
                      class="status-dot"
                    ></span>
                    <h2 class="truncate text-base font-semibold tracking-[-.025em]">
                      {node.registration.name}
                    </h2>
                    <span class={`status-badge ${node.registration.status}`}
                      >{statusLabel(node.registration.status)}</span
                    >
                  </div>
                  <p class="mt-1 truncate font-mono text-[11px] text-zinc-400">
                    {node.registration.backendId}
                  </p>
                </div>
                <span
                  class={`rounded-full px-2 py-1 text-[10px] font-bold ${node.online ? 'bg-emerald-50 text-emerald-700 dark:bg-emerald-500/10 dark:text-emerald-300' : 'bg-zinc-100 text-zinc-500 dark:bg-zinc-800'}`}
                  >{node.online ? text.online : text.offline}</span
                >
              </div>

              <div class="mt-5 grid grid-cols-2 gap-2.5">
                <div class="metric">
                  <span class="metric-label"
                    ><svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="1.8"
                      class="h-3.5 w-3.5"
                      aria-hidden="true"
                      ><path d="M12 21s7-5.2 7-12A7 7 0 0 0 5 9c0 6.8 7 12 7 12Z" /><circle
                        cx="12"
                        cy="9"
                        r="2"
                      /></svg
                    >{text.ip}</span
                  ><strong class="truncate font-mono text-xs">{node.registration.ip ?? '—'}</strong>
                </div>
                <div class="metric">
                  <span class="metric-label"
                    ><svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="1.8"
                      class="h-3.5 w-3.5"
                      aria-hidden="true"
                      ><circle cx="12" cy="12" r="8" /><path
                        d="M4 12h16M12 4c2 2.2 3 5 3 8s-1 5.8-3 8c-2-2.2-3-5-3-8s1-5.8 3-8Z"
                      /></svg
                    >{text.location}</span
                  ><strong class="truncate text-xs">{node.registration.location ?? '—'}</strong>
                </div>
                <div class="metric">
                  <span class="metric-label"
                    ><svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="1.8"
                      class="h-3.5 w-3.5"
                      aria-hidden="true"
                      ><rect x="7" y="7" width="10" height="10" rx="1" /><path
                        d="M9 2v5m6-5v5M9 17v5m6-5v5M2 9h5m10 0h5M2 15h5m10 0h5"
                      /></svg
                    >{text.cpu}</span
                  ><strong class="text-xs">{cpuValue(node)}</strong>
                </div>
                <div class="metric">
                  <span class="metric-label"
                    ><svg
                      viewBox="0 0 24 24"
                      fill="none"
                      stroke="currentColor"
                      stroke-width="1.8"
                      class="h-3.5 w-3.5"
                      aria-hidden="true"
                      ><rect x="5" y="4" width="14" height="16" rx="2" /><path
                        d="M9 8h6m-6 4h6m-6 4h3"
                      /></svg
                    >{text.memory}</span
                  ><strong class="text-xs"
                    >{#if memoryPercent(node) !== undefined}{memoryPercent(node)}%
                      <span class="font-normal text-zinc-400"
                        >{bytes(node.status?.metrics?.memory.usedBytes)}</span
                      >{:else}—{/if}</strong
                  >
                </div>
              </div>

              <div class="mt-3 flex flex-wrap gap-1.5">
                {#each node.registration.tags as tag}<span
                    class="rounded-full bg-blue-50 px-2 py-0.5 text-[10px] font-semibold text-blue-600 dark:bg-blue-500/10 dark:text-blue-300"
                    >{tag}</span
                  >{/each}
              </div>
              <p class="mt-3 truncate text-[11px] text-zinc-400">
                {node.status
                  ? `${text.checked} · ${displayDate(node.checkedAt)}`
                  : text.unavailable}
              </p>

              <div
                class="mt-auto flex items-center justify-between gap-2 border-t border-zinc-100 pt-4 dark:border-white/[.07]"
              >
                <span class="truncate text-[11px] text-zinc-400">{node.registration.baseUrl}</span>
                <div class="flex shrink-0 items-center gap-1.5">
                  {#if node.registration.status === 'pending'}
                    <button
                      class="icon-action success"
                      title={text.approve}
                      aria-label={text.approve}
                      onclick={() => approve(node)}
                      disabled={actingId === node.registration.id}
                      ><svg
                        viewBox="0 0 24 24"
                        fill="none"
                        stroke="currentColor"
                        stroke-width="2.2"
                        class="h-4 w-4"
                        aria-hidden="true"><path d="m5 12 4 4L19 6" /></svg
                      ></button
                    >
                    <button
                      class="icon-action danger"
                      title={text.reject}
                      aria-label={text.reject}
                      onclick={() => reject(node)}
                      disabled={actingId === node.registration.id}
                      ><svg
                        viewBox="0 0 24 24"
                        fill="none"
                        stroke="currentColor"
                        stroke-width="2"
                        class="h-4 w-4"
                        aria-hidden="true"><path d="m7 7 10 10M17 7 7 17" /></svg
                      ></button
                    >
                  {:else if node.backend}
                    <button
                      class="icon-action"
                      title={text.test}
                      aria-label={text.test}
                      onclick={() => testBackend(node)}
                      disabled={actingId === node.backend.id}
                      ><svg
                        viewBox="0 0 24 24"
                        fill="none"
                        stroke="currentColor"
                        stroke-width="1.9"
                        class="h-4 w-4"
                        aria-hidden="true"><path d="M20 11a8 8 0 1 0 2 5.3M20 4v7h-7" /></svg
                      ></button
                    >
                    <details class="relative">
                      <summary
                        class="icon-action list-none"
                        title={text.actions}
                        aria-label={text.actions}
                        ><svg
                          viewBox="0 0 24 24"
                          fill="currentColor"
                          class="h-4 w-4"
                          aria-hidden="true"
                          ><circle cx="5" cy="12" r="1.5" /><circle
                            cx="12"
                            cy="12"
                            r="1.5"
                          /><circle cx="19" cy="12" r="1.5" /></svg
                        ></summary
                      >
                      <div class="action-menu">
                        <button
                          class="text-rose-600 dark:text-rose-300"
                          onclick={() => deleteBackend(node)}>{text.remove}</button
                        >
                      </div>
                    </details>
                  {/if}
                </div>
              </div>
            </article>
          {/each}
        </section>
      {/if}
    {:else}
      <section class="mb-5 flex items-center justify-between gap-3">
        <div>
          <h1 class="text-2xl font-semibold tracking-[-.045em]">{text.installer}</h1>
          <p class="mt-1 text-sm text-zinc-500 dark:text-zinc-400">{text.autoId}</p>
        </div>
        <button
          class="icon-button"
          title={text.backToNodes}
          aria-label={text.backToNodes}
          onclick={() => (activeView = 'fleet')}
          ><svg
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            stroke-width="1.8"
            class="h-4 w-4"
            aria-hidden="true"><path d="m14 5-7 7 7 7M7 12h12" /></svg
          ></button
        >
      </section>
      <div class="grid gap-5 xl:grid-cols-[minmax(0,1fr)_minmax(390px,.9fr)]">
        <section class="surface-card p-5 sm:p-7">
          <p class="mb-5 text-sm text-zinc-500 dark:text-zinc-400">{text.setupTunnel}</p>
          <div class="mb-5 grid gap-3 sm:grid-cols-2">
            <button
              class:mode-selected={installTunnelMode === 'managed'}
              class="tunnel-mode-card text-left"
              onclick={() => (installTunnelMode = 'managed')}
            >
              <span class="flex items-center justify-between gap-3"
                ><strong>{text.managedTunnel}</strong><svg
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="1.8"
                  class="h-4 w-4"
                  aria-hidden="true"
                  ><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10Z" /><path
                    d="m9 12 2 2 4-4"
                  /></svg
                ></span
              >
              <span>{text.managedTunnelDescription}</span>
            </button>
            <button
              class:mode-selected={installTunnelMode === 'quick'}
              class="tunnel-mode-card text-left"
              onclick={() => (installTunnelMode = 'quick')}
            >
              <span class="flex items-center justify-between gap-3"
                ><strong>{text.quickTunnel}</strong><svg
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  stroke-width="1.8"
                  class="h-4 w-4"
                  aria-hidden="true"
                  ><path d="M12 4v16M4 12h16M5.8 5.8l12.4 12.4M18.2 5.8 5.8 18.2" /></svg
                ></span
              >
              <span>{text.quickTunnelDescription}</span>
            </button>
          </div>
          <div class="grid gap-4 sm:grid-cols-2">
            <label class="field-label"
              >{text.nodeName}<input
                bind:value={installBackendName}
                class="field-input"
                autocomplete="off"
                placeholder="web-01"
              /></label
            ><label class="field-label"
              >{text.tags}<input
                bind:value={installTags}
                class="field-input"
                placeholder="production,full"
              /></label
            ><label class="field-label sm:col-span-2"
              >{text.redisUrl}<input
                bind:value={installRedisUrl}
                class="field-input"
                type="password"
                autocomplete="off"
                placeholder="rediss://default:password@host:port"
              /><span class="text-[11px] font-normal leading-4 text-zinc-400"
                >{text.redisUrlHint}</span
              ></label
            ><label class="field-label sm:col-span-2"
              >{text.registrationSecret}<input
                bind:value={installRegistrationSecret}
                class="field-input"
                type="password"
                autocomplete="off"
                placeholder="<REGISTRATION_SECRET>"
              /><span class="text-[11px] font-normal leading-4 text-zinc-400"
                >{text.registrationSecretHint}</span
              ></label
            >
          </div>
          {#if installTunnelMode === 'managed'}
            <div class="mt-5 rounded-2xl bg-blue-50 p-4 dark:bg-blue-500/10">
              {#if managedProvision}
                <p class="text-xs font-semibold text-blue-700 dark:text-blue-200">
                  {text.managedTunnelReady}
                </p>
                <p class="mt-1 truncate font-mono text-xs text-blue-600 dark:text-blue-300">
                  {managedProvision.publicUrl}
                </p>
              {:else}
                <button
                  class="primary-button"
                  onclick={provisionManagedTunnel}
                  disabled={provisioningTunnel}
                  >{provisioningTunnel ? text.provisioningTunnel : text.provisionTunnel}</button
                >
              {/if}
            </div>
          {:else}
            <p
              class="mt-5 rounded-2xl bg-amber-50 px-4 py-3 text-xs leading-5 text-amber-700 dark:bg-amber-500/10 dark:text-amber-200"
            >
              {text.quickTunnelNotice}
            </p>
          {/if}
          <label
            class="mt-5 flex cursor-pointer items-start gap-3 rounded-2xl bg-rose-50 px-4 py-3 text-xs text-rose-700 dark:bg-rose-500/10 dark:text-rose-200"
            ><input
              bind:checked={installAllowApt}
              class="mt-0.5 h-4 w-4 accent-rose-600"
              type="checkbox"
            /><span><strong>{text.allowApt}</strong><br />{text.allowAptHint}</span></label
          >
        </section>
        <aside
          class="overflow-hidden rounded-[28px] bg-[#161617] text-white shadow-[0_18px_44px_rgba(0,0,0,.18)]"
        >
          <div class="flex items-center justify-between border-b border-white/10 px-5 py-4">
            <div>
              <p class="text-[10px] font-bold tracking-[.14em] text-blue-300">RUN ON VPS</p>
              <p class="mt-1 text-sm font-semibold">{text.installer}</p>
            </div>
            <button
              class="rounded-full bg-white/10 px-3 py-1.5 text-xs font-semibold transition hover:bg-white/15"
              onclick={() => copyToClipboard(installCommand, text.copied)}>{text.copy}</button
            >
          </div>
          <pre
            class="max-h-125 overflow-auto p-5 font-mono text-xs leading-6 text-zinc-200">{installCommand}</pre>
          <div
            class="flex items-center justify-between border-t border-white/10 px-5 py-4 text-xs text-zinc-400"
          >
            <span>{text.noTask}</span><a
              class="rounded-full bg-white px-3 py-1.5 font-semibold text-zinc-900 hover:bg-zinc-100"
              href="/install-agent.sh"
              download>{text.download}</a
            >
          </div>
        </aside>
      </div>
    {/if}
  </main>

  <footer
    class="mx-auto flex max-w-7xl items-center justify-between gap-4 px-4 pb-7 text-xs text-zinc-400 sm:px-6 lg:px-8"
  >
    <span>VPS Agent · Cloudflare Worker + D1 + BullMQ</span>
    <div class="flex gap-4">
      <button
        class="hover:text-zinc-700 dark:hover:text-zinc-200"
        onclick={() => copyToClipboard(`${origin}/mcp`, text.mcpCopied)}>MCP</button
      ><a
        class="hover:text-zinc-700 dark:hover:text-zinc-200"
        href="https://github.com/Ykmmj/vps-agent-platform"
        target="_blank"
        rel="noreferrer">GitHub</a
      >
    </div>
  </footer>
</div>
