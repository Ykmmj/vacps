<script lang="ts">
  import { onMount } from 'svelte';

  type RegistrationStatus = 'pending' | 'approved' | 'rejected';

  interface Backend {
    id: string;
    name: string;
    baseUrl: string;
    region?: string;
    tags: string[];
    enabled: boolean;
  }

  interface Registration {
    id: string;
    backendId: string;
    name: string;
    baseUrl: string;
    region?: string;
    tags: string[];
    agentVersion: string;
    status: RegistrationStatus;
    requestedAt: string;
    updatedAt: string;
    decisionAt?: string;
    rejectionReason?: string;
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
    backends: Backend[];
    pendingRegistrations: Registration[];
  }

  const origin = window.location.origin;
  const repositoryUrl = 'https://github.com/Ykmmj/vps-agent-platform.git';

  let dashboard: Dashboard | undefined;
  let registrations: Registration[] = [];
  let loading = true;
  let notice = '正在同步控制平面…';
  let noticeTone: 'default' | 'error' | 'success' = 'default';
  let activeView: 'fleet' | 'install' = 'fleet';
  let registrationFilter: 'pending' | 'all' = 'pending';
  let actingId: string | undefined;

  let installBackendId = 'vps-la-01';
  let installBackendName = 'Los Angeles VPS';
  let installPublicUrl = 'https://agent.example.com';
  let installRegion = 'us-west';
  let installTags = 'production,full';
  let installRedisUrl = '';
  let installBackendToken = '';
  let installTunnelToken = '';
  let installAllowApt = false;

  $: pendingRegistrations = registrations.filter(
    (registration) => registration.status === 'pending',
  );
  $: visibleRegistrations = registrationFilter === 'pending' ? pendingRegistrations : registrations;
  $: installCommand = buildInstallCommand();

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
      const [nextDashboard, nextRegistrations] = await Promise.all([
        api<Dashboard>('/api/dashboard'),
        api<Registration[]>('/api/registrations'),
      ]);
      dashboard = nextDashboard;
      registrations = nextRegistrations;
      setNotice('控制平面已同步', 'success');
    } catch (error) {
      setNotice(`同步失败：${messageOf(error)}`, 'error');
    } finally {
      loading = false;
    }
  }

  async function approve(registration: Registration) {
    actingId = registration.id;
    try {
      await api(`/api/registrations/${registration.id}/approve`, { method: 'POST' });
      setNotice(`${registration.name} 已获批准并启用`, 'success');
      await refresh();
    } catch (error) {
      setNotice(`审批未完成：${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function reject(registration: Registration) {
    const reason = window.prompt('可选：填写拒绝原因，Agent 下次注册会重新进入待审批状态。');
    if (reason === null) return;
    actingId = registration.id;
    try {
      await api(`/api/registrations/${registration.id}/reject`, {
        method: 'POST',
        body: JSON.stringify({ reason: reason.trim() || undefined }),
      });
      setNotice(`${registration.name} 已被拒绝`, 'default');
      await refresh();
    } catch (error) {
      setNotice(`拒绝操作失败：${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function testBackend(backend: Backend) {
    actingId = backend.id;
    try {
      await api(`/api/backends/${backend.id}/test`, { method: 'POST' });
      setNotice(`${backend.name} 健康检查通过`, 'success');
    } catch (error) {
      setNotice(`节点不可达：${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  async function deleteBackend(backend: Backend) {
    if (!window.confirm(`确定移除 ${backend.name}？此操作不会卸载 VPS 上的 Agent。`)) return;
    actingId = backend.id;
    try {
      await api(`/api/backends/${backend.id}`, { method: 'DELETE' });
      setNotice(`${backend.name} 已从控制平面移除`, 'default');
      await refresh();
    } catch (error) {
      setNotice(`移除失败：${messageOf(error)}`, 'error');
    } finally {
      actingId = undefined;
    }
  }

  function buildInstallCommand() {
    const lines = [
      `curl -fsSL ${origin}/install-agent.sh | sudo bash -s -- \\`,
      `  --repo ${shellQuote(repositoryUrl)} \\`,
      `  --backend-id ${shellQuote(installBackendId || 'vps-la-01')} \\`,
      `  --backend-name ${shellQuote(installBackendName || installBackendId || 'VPS Agent')} \\`,
      `  --control-plane-url ${shellQuote(origin)} \\`,
      `  --public-url ${shellQuote(installPublicUrl || 'https://agent.example.com')} \\`,
      `  --backend-token ${shellQuote(installBackendToken || '<BACKEND_SHARED_TOKEN>')} \\`,
      `  --redis-url ${shellQuote(installRedisUrl || '<REDIS_TLS_URL>')}`,
    ];
    if (installRegion) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --region ${shellQuote(installRegion)}`);
    }
    if (installTags) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --tags ${shellQuote(installTags)}`);
    }
    if (installTunnelToken) {
      lines[lines.length - 1] += ' \\';
      lines.push(`  --tunnel-token ${shellQuote(installTunnelToken)}`);
    }
    if (installAllowApt) {
      lines[lines.length - 1] += ' \\';
      lines.push('  --allow-apt');
    }
    return lines.join('\n');
  }

  async function copy(text: string, successMessage: string) {
    try {
      await navigator.clipboard.writeText(text);
      setNotice(successMessage, 'success');
    } catch {
      setNotice('无法访问剪贴板，请手动复制。', 'error');
    }
  }

  function shellQuote(value: string) {
    return `'${value.replaceAll("'", "'\\''")}'`;
  }

  function displayDate(value: string) {
    return new Intl.DateTimeFormat('zh-CN', { dateStyle: 'medium', timeStyle: 'short' }).format(
      new Date(value),
    );
  }

  function messageOf(error: unknown) {
    return error instanceof Error ? error.message : String(error);
  }

  function registrationIconClasses(status: RegistrationStatus) {
    return status === 'pending'
      ? 'bg-amber-50 text-amber-600'
      : status === 'approved'
        ? 'bg-emerald-50 text-emerald-600'
        : 'bg-rose-50 text-rose-600';
  }

  function registrationBadgeClasses(status: RegistrationStatus) {
    return status === 'pending'
      ? 'bg-amber-100 text-amber-700'
      : status === 'approved'
        ? 'bg-emerald-100 text-emerald-700'
        : 'bg-rose-100 text-rose-700';
  }

  onMount(() => void refresh());
</script>

<svelte:head>
  <title>VPS Agent Control</title>
</svelte:head>

<div class="min-h-screen bg-[#f5f5f7] text-[#1d1d1f] apple-grid">
  <header class="sticky top-0 z-20 border-b border-black/[.055] bg-white/80 backdrop-blur-xl">
    <div class="mx-auto flex h-16 max-w-7xl items-center justify-between px-5 lg:px-8">
      <button class="flex items-center gap-3 text-left" onclick={() => (activeView = 'fleet')}>
        <span
          class="grid h-8 w-8 place-items-center rounded-[11px] bg-[#1d1d1f] text-sm font-bold text-white"
          >V</span
        >
        <span
          ><strong class="block text-sm tracking-[-.02em]">VPS Agent</strong><span
            class="block text-[10px] font-medium tracking-[.12em] text-zinc-400">CONTROL PLANE</span
          ></span
        >
      </button>
      <div
        class="hidden items-center gap-2 rounded-full border border-black/[.055] bg-zinc-100 p-1 sm:flex"
      >
        <button
          class:!bg-white={activeView === 'fleet'}
          class:shadow-sm={activeView === 'fleet'}
          class="rounded-full px-4 py-1.5 text-xs font-semibold text-zinc-600 transition"
          onclick={() => (activeView = 'fleet')}>节点审批</button
        >
        <button
          class:!bg-white={activeView === 'install'}
          class:shadow-sm={activeView === 'install'}
          class="rounded-full px-4 py-1.5 text-xs font-semibold text-zinc-600 transition"
          onclick={() => (activeView = 'install')}>安装 Agent</button
        >
      </div>
      <div class="flex items-center gap-2 text-xs font-medium">
        <span
          class="h-2 w-2 rounded-full"
          class:bg-emerald-500={noticeTone !== 'error'}
          class:bg-rose-500={noticeTone === 'error'}
        ></span>
        <span class="hidden text-zinc-500 md:inline">{notice}</span>
        <button
          class="rounded-full bg-zinc-100 px-3 py-1.5 text-zinc-600 transition hover:bg-zinc-200"
          onclick={refresh}
          disabled={loading}>刷新</button
        >
      </div>
    </div>
  </header>

  <main class="mx-auto max-w-7xl px-5 py-8 lg:px-8 lg:py-11">
    {#if activeView === 'fleet'}
      <section class="mb-8 flex flex-col gap-5 lg:flex-row lg:items-end lg:justify-between">
        <div>
          <p class="mb-2 text-xs font-bold tracking-[.15em] text-[#0071e3]">FLEET OPERATIONS</p>
          <h1 class="text-4xl font-semibold tracking-[-.055em] text-[#1d1d1f] sm:text-5xl">
            审批每一次节点接入。
          </h1>
          <p class="mt-3 max-w-2xl text-sm leading-6 text-zinc-500">
            VPS 安装完成后会自行发起注册；只有在这里审批且健康检查通过，节点才会进入 AI 调度网络。
          </p>
        </div>
        <button
          class="inline-flex items-center justify-center rounded-full bg-[#0071e3] px-5 py-3 text-sm font-semibold text-white shadow-lg shadow-blue-500/20 transition hover:bg-[#0077ed]"
          onclick={() => (activeView = 'install')}
          >安装新的 Agent <span class="ml-2 text-lg leading-none">+</span></button
        >
      </section>

      <section class="mb-7 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
        <article class="surface-shadow rounded-[24px] bg-white p-5">
          <p class="text-xs font-medium text-zinc-500">待审批请求</p>
          <p class="mt-3 text-4xl font-semibold tracking-[-.06em]">
            {dashboard?.totals.pendingRegistrations ?? '—'}
          </p>
          <p class="mt-2 text-xs text-amber-600">需要你的确认</p>
        </article>
        <article class="surface-shadow rounded-[24px] bg-white p-5">
          <p class="text-xs font-medium text-zinc-500">已启用节点</p>
          <p class="mt-3 text-4xl font-semibold tracking-[-.06em]">
            {dashboard?.totals.enabledBackends ?? '—'}
          </p>
          <p class="mt-2 text-xs text-emerald-600">可接收 MCP 任务</p>
        </article>
        <article class="surface-shadow rounded-[24px] bg-white p-5">
          <p class="text-xs font-medium text-zinc-500">运行中的工作流</p>
          <p class="mt-3 text-4xl font-semibold tracking-[-.06em]">
            {dashboard?.totals.active ?? '—'}
          </p>
          <p class="mt-2 text-xs text-zinc-400">由 MCP 或计划触发</p>
        </article>
        <article class="surface-shadow rounded-[24px] bg-[#1d1d1f] p-5 text-white">
          <p class="text-xs font-medium text-zinc-400">安全提示</p>
          <p class="mt-3 text-base font-semibold tracking-[-.02em]">先保护，再审批。</p>
          <a
            class="mt-2 inline-block text-xs text-blue-300 hover:text-blue-200"
            href="https://dash.cloudflare.com/"
            target="_blank"
            rel="noreferrer">为此域名配置 Cloudflare Access ↗</a
          >
        </article>
      </section>

      <section class="surface-shadow overflow-hidden rounded-[28px] bg-white">
        <div
          class="flex flex-col gap-4 border-b border-zinc-100 px-5 py-5 sm:flex-row sm:items-center sm:justify-between sm:px-7"
        >
          <div>
            <h2 class="text-xl font-semibold tracking-[-.04em]">注册审批队列</h2>
            <p class="mt-1 text-xs text-zinc-500">审批时将验证 Tunnel 地址与 Agent 身份。</p>
          </div>
          <div class="flex rounded-full bg-zinc-100 p-1 text-xs font-semibold">
            <button
              class:!bg-white={registrationFilter === 'pending'}
              class:shadow-sm={registrationFilter === 'pending'}
              class="rounded-full px-3 py-1.5 text-zinc-600"
              onclick={() => (registrationFilter = 'pending')}
              >待处理 {pendingRegistrations.length}</button
            ><button
              class:!bg-white={registrationFilter === 'all'}
              class:shadow-sm={registrationFilter === 'all'}
              class="rounded-full px-3 py-1.5 text-zinc-600"
              onclick={() => (registrationFilter = 'all')}>全部</button
            >
          </div>
        </div>
        <div class="divide-y divide-zinc-100">
          {#if loading && !dashboard}
            <div class="p-10 text-center text-sm text-zinc-400">正在读取注册请求…</div>
          {:else if visibleRegistrations.length === 0}
            <div class="p-10 text-center">
              <div
                class="mx-auto grid h-12 w-12 place-items-center rounded-2xl bg-zinc-100 text-xl"
              >
                ✓
              </div>
              <p class="mt-3 text-sm font-semibold">
                {registrationFilter === 'pending' ? '没有待审批的节点' : '尚无注册记录'}
              </p>
              <p class="mt-1 text-xs text-zinc-500">
                在右上角生成安装命令，VPS 启动后会自动出现在这里。
              </p>
            </div>
          {:else}
            {#each visibleRegistrations as registration}
              <article
                class="flex flex-col gap-5 p-5 sm:p-7 lg:flex-row lg:items-center lg:justify-between"
              >
                <div class="flex min-w-0 gap-4">
                  <div
                    class={`grid h-11 w-11 shrink-0 place-items-center rounded-2xl ${registrationIconClasses(registration.status)}`}
                  >
                    {registration.status === 'pending'
                      ? '↗'
                      : registration.status === 'approved'
                        ? '✓'
                        : '×'}
                  </div>
                  <div class="min-w-0">
                    <div class="flex flex-wrap items-center gap-2">
                      <h3 class="font-semibold tracking-[-.02em]">{registration.name}</h3>
                      <span
                        class={`rounded-full px-2 py-0.5 text-[10px] font-bold ${registrationBadgeClasses(registration.status)}`}
                        >{registration.status === 'pending'
                          ? '待审批'
                          : registration.status === 'approved'
                            ? '已批准'
                            : '已拒绝'}</span
                      >
                    </div>
                    <p class="mt-1 truncate font-mono text-xs text-zinc-500">
                      {registration.baseUrl}
                    </p>
                    <div class="mt-2 flex flex-wrap gap-1.5">
                      <span
                        class="rounded-full bg-zinc-100 px-2 py-0.5 text-[10px] font-medium text-zinc-500"
                        >{registration.backendId}</span
                      >{#if registration.region}<span
                          class="rounded-full bg-zinc-100 px-2 py-0.5 text-[10px] font-medium text-zinc-500"
                          >{registration.region}</span
                        >{/if}{#each registration.tags as tag}<span
                          class="rounded-full bg-blue-50 px-2 py-0.5 text-[10px] font-medium text-blue-600"
                          >{tag}</span
                        >{/each}
                    </div>
                    <p class="mt-2 text-[11px] text-zinc-400">
                      请求于 {displayDate(registration.requestedAt)} · Agent {registration.agentVersion}
                    </p>
                    {#if registration.rejectionReason}<p class="mt-2 text-xs text-rose-600">
                        拒绝原因：{registration.rejectionReason}
                      </p>{/if}
                  </div>
                </div>
                {#if registration.status === 'pending'}
                  <div class="flex shrink-0 gap-2">
                    <button
                      class="rounded-full border border-zinc-200 px-4 py-2 text-xs font-semibold text-zinc-600 hover:border-zinc-300 hover:bg-zinc-50"
                      onclick={() => reject(registration)}
                      disabled={actingId === registration.id}>拒绝</button
                    ><button
                      class="rounded-full bg-[#0071e3] px-4 py-2 text-xs font-semibold text-white shadow-md shadow-blue-500/20 hover:bg-[#0077ed] disabled:opacity-50"
                      onclick={() => approve(registration)}
                      disabled={actingId === registration.id}
                      >{actingId === registration.id ? '验证中…' : '批准并启用'}</button
                    >
                  </div>
                {/if}
              </article>
            {/each}
          {/if}
        </div>
      </section>

      <section class="surface-shadow mt-6 overflow-hidden rounded-[28px] bg-white">
        <div class="flex items-center justify-between border-b border-zinc-100 px-5 py-5 sm:px-7">
          <div>
            <h2 class="text-xl font-semibold tracking-[-.04em]">已启用节点</h2>
            <p class="mt-1 text-xs text-zinc-500">
              任务只通过 MCP 或计划工作流提交，WebUI 不直接创建任务。
            </p>
          </div>
          <span class="rounded-full bg-zinc-100 px-3 py-1 text-xs font-semibold text-zinc-500"
            >{dashboard?.backends.length ?? 0} nodes</span
          >
        </div>
        <div class="overflow-x-auto">
          <table class="w-full min-w-175 text-left">
            <thead class="bg-zinc-50 text-[10px] font-bold tracking-[.1em] text-zinc-400"
              ><tr
                ><th class="px-5 py-3 sm:px-7">节点</th><th class="px-5 py-3">端点</th><th
                  class="px-5 py-3">区域 / 标签</th
                ><th class="px-5 py-3 text-right sm:px-7">操作</th></tr
              ></thead
            ><tbody class="divide-y divide-zinc-100"
              >{#if dashboard?.backends.length}{#each dashboard.backends as backend}<tr
                    ><td class="px-5 py-4 sm:px-7"
                      ><p class="text-sm font-semibold">{backend.name}</p>
                      <p class="mt-1 font-mono text-[11px] text-zinc-400">{backend.id}</p></td
                    ><td class="max-w-64 truncate px-5 py-4 font-mono text-xs text-zinc-500"
                      >{backend.baseUrl}</td
                    ><td class="px-5 py-4"
                      ><div class="flex flex-wrap gap-1.5">
                        {#if backend.region}<span
                            class="rounded-full bg-zinc-100 px-2 py-0.5 text-[10px] font-medium text-zinc-500"
                            >{backend.region}</span
                          >{/if}{#each backend.tags as tag}<span
                            class="rounded-full bg-blue-50 px-2 py-0.5 text-[10px] font-medium text-blue-600"
                            >{tag}</span
                          >{/each}
                      </div></td
                    ><td class="px-5 py-4 text-right sm:px-7"
                      ><button
                        class="mr-2 rounded-full px-3 py-1.5 text-xs font-semibold text-zinc-600 hover:bg-zinc-100"
                        onclick={() => testBackend(backend)}
                        disabled={actingId === backend.id}>测试</button
                      ><button
                        class="rounded-full px-3 py-1.5 text-xs font-semibold text-rose-600 hover:bg-rose-50"
                        onclick={() => deleteBackend(backend)}
                        disabled={actingId === backend.id}>移除</button
                      ></td
                    ></tr
                  >{/each}{:else}<tr
                  ><td class="px-5 py-9 text-center text-sm text-zinc-400" colspan="4"
                    >批准第一个注册请求后，节点会出现在这里。</td
                  ></tr
                >{/if}</tbody
            >
          </table>
        </div>
      </section>
    {:else}
      <section class="mb-8">
        <p class="mb-2 text-xs font-bold tracking-[.15em] text-[#0071e3]">AGENT INSTALLER</p>
        <h1 class="text-4xl font-semibold tracking-[-.055em] sm:text-5xl">生成一条安装命令。</h1>
        <p class="mt-3 max-w-2xl text-sm leading-6 text-zinc-500">
          安装器会把节点注册到当前控制平面；安装成功后回到审批队列确认接入。这里不会提交或保存你的
          Token。
        </p>
      </section>
      <div class="grid gap-6 xl:grid-cols-[minmax(0,1fr)_minmax(420px,.9fr)]">
        <section class="surface-shadow rounded-[28px] bg-white p-5 sm:p-7">
          <div class="mb-6 flex items-start gap-3">
            <span
              class="grid h-9 w-9 shrink-0 place-items-center rounded-2xl bg-blue-50 text-sm font-bold text-[#0071e3]"
              >1</span
            >
            <div>
              <h2 class="text-lg font-semibold tracking-[-.03em]">准备节点信息</h2>
              <p class="mt-1 text-xs leading-5 text-zinc-500">
                请先在 Cloudflare Tunnel 中创建公开主机名，并路由到 <code
                  class="rounded bg-zinc-100 px-1 py-0.5">http://127.0.0.1:3100</code
                >。
              </p>
            </div>
          </div>
          <div class="grid gap-4 sm:grid-cols-2">
            <label class="field-label"
              >Backend ID<input
                bind:value={installBackendId}
                class="field-input"
                pattern="[a-z0-9-]+"
              /></label
            ><label class="field-label"
              >显示名称<input bind:value={installBackendName} class="field-input" /></label
            ><label class="field-label sm:col-span-2"
              >公开 Agent URL<input
                bind:value={installPublicUrl}
                class="field-input"
                type="url"
                placeholder="https://agent.example.com"
              /></label
            ><label class="field-label"
              >区域<input
                bind:value={installRegion}
                class="field-input"
                placeholder="us-west"
              /></label
            ><label class="field-label"
              >标签（逗号分隔）<input
                bind:value={installTags}
                class="field-input"
                placeholder="production,full"
              /></label
            ><label class="field-label sm:col-span-2"
              >Redis TLS URL<input
                bind:value={installRedisUrl}
                class="field-input"
                type="password"
                autocomplete="off"
                placeholder="rediss://default:password@host:port"
              /></label
            ><label class="field-label sm:col-span-2"
              >Backend Shared Token<input
                bind:value={installBackendToken}
                class="field-input"
                type="password"
                autocomplete="off"
                placeholder="Cloudflare 部署输出的 BACKEND_SHARED_TOKEN"
              /></label
            ><label class="field-label sm:col-span-2"
              >Cloudflare Tunnel Token <span class="font-normal text-zinc-400">可选</span><input
                bind:value={installTunnelToken}
                class="field-input"
                type="password"
                autocomplete="off"
                placeholder="从 Tunnel 页面复制安装 Token"
              /></label
            >
          </div>
          <label
            class="mt-5 flex cursor-pointer items-start gap-3 rounded-2xl bg-rose-50 px-4 py-3 text-xs text-rose-700"
            ><input
              bind:checked={installAllowApt}
              class="mt-0.5 h-4 w-4 accent-rose-600"
              type="checkbox"
            /><span
              ><strong>允许 Agent 安装 apt 软件包</strong><br />这会允许包维护脚本以 root
              权限运行，仅在明确需要时启用。</span
            ></label
          >
        </section>
        <aside class="surface-shadow overflow-hidden rounded-[28px] bg-[#1d1d1f] text-white">
          <div class="flex items-center justify-between border-b border-white/10 px-5 py-4">
            <div>
              <p class="text-[10px] font-bold tracking-[.14em] text-blue-300">02 / RUN ON VPS</p>
              <p class="mt-1 text-sm font-semibold">安装并等待审批</p>
            </div>
            <button
              class="rounded-full bg-white/10 px-3 py-1.5 text-xs font-semibold text-white transition hover:bg-white/15"
              onclick={() => copy(installCommand, '安装命令已复制')}>复制命令</button
            >
          </div>
          <pre
            class="max-h-110 overflow-auto p-5 font-mono text-xs leading-6 text-zinc-200">{installCommand}</pre>
          <div class="border-t border-white/10 p-5">
            <a
              class="inline-flex items-center rounded-full bg-white px-4 py-2 text-xs font-bold text-[#1d1d1f] hover:bg-zinc-100"
              href="/install-agent.sh"
              download>下载 install-agent.sh <span class="ml-2">↓</span></a
            >
            <ol class="mt-5 space-y-3 text-xs leading-5 text-zinc-400">
              <li><span class="mr-2 text-blue-300">01</span>在目标 VPS 粘贴并执行上方命令。</li>
              <li>
                <span class="mr-2 text-blue-300">02</span>Agent 启动后会自动向
                <code class="text-zinc-200">{origin}/api/registrations</code> 发起注册。
              </li>
              <li>
                <span class="mr-2 text-blue-300">03</span>返回“节点审批”，验证 URL 后批准接入。
              </li>
            </ol>
          </div>
        </aside>
      </div>
    {/if}
  </main>

  <footer
    class="mx-auto flex max-w-7xl flex-col gap-2 px-5 pb-8 text-xs text-zinc-400 sm:flex-row sm:items-center sm:justify-between lg:px-8"
  >
    <span>VPS Agent Control · Cloudflare Worker + D1 + BullMQ</span>
    <div class="flex gap-4">
      <button class="hover:text-zinc-600" onclick={() => copy(`${origin}/mcp`, 'MCP 地址已复制')}
        >复制 MCP 地址</button
      ><a
        class="hover:text-zinc-600"
        href="https://github.com/Ykmmj/vps-agent-platform"
        target="_blank"
        rel="noreferrer">GitHub ↗</a
      >
    </div>
  </footer>
</div>
