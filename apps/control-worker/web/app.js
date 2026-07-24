const $ = (selector) => document.querySelector(selector);
const notice = $('#notice');
const origin = window.location.origin;

const request = async (url, init = {}) => {
  const headers = new Headers(init.headers);
  headers.set('content-type', 'application/json');
  const response = await fetch(url, { ...init, headers });
  if (response.status === 204) return null;
  const body = await response.json();
  if (!response.ok) throw new Error(body.error?.message ?? `HTTP ${response.status}`);
  return body;
};

const escape = (value) =>
  String(value).replace(
    /[&<>'"]/g,
    (character) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[character],
  );

const setNotice = (message, isError = false) => {
  notice.innerHTML = `<span class="pulse"></span>${escape(message)}`;
  notice.style.color = isError ? '#ff9eab' : '';
};

const statusLabel = (status) =>
  ({
    queued: '队列中',
    running: '执行中',
    succeeded: '已完成',
    failed: '失败',
    dispatch_failed: '分发失败',
    timed_out: '已超时',
  })[status] ?? status;

const shellQuote = (value) => `'${String(value).replaceAll("'", "'\\''")}'`;

function updateInstallerCommand() {
  const backendId = $('#install-backend-id').value.trim() || 'vps-la-01';
  const redisUrl = $('#install-redis-url').value.trim() || '<Redis TLS URL>';
  const backendToken = $('#install-backend-token').value.trim() || '<BACKEND_SHARED_TOKEN>';
  const tunnelToken = $('#install-tunnel-token').value.trim();
  const repository =
    $('#install-repo').value.trim() || 'https://github.com/Ykmmj/vps-agent-platform.git';
  const lines = [
    `curl -fsSL ${origin}/install-agent.sh | sudo bash -s -- \\`,
    `  --repo ${shellQuote(repository)} \\`,
    `  --backend-id ${shellQuote(backendId)} \\`,
    `  --backend-token ${shellQuote(backendToken)} \\`,
    `  --redis-url ${shellQuote(redisUrl)}`,
  ];
  if (tunnelToken) lines[lines.length - 1] += ' \\';
  if (tunnelToken) lines.push(`  --tunnel-token ${shellQuote(tunnelToken)}`);
  if ($('#install-allow-apt').checked) {
    lines[lines.length - 1] += ' \\';
    lines.push('  --allow-apt');
  }
  $('#install-command').textContent = lines.join('\n');
}

async function copyText(text, successMessage) {
  try {
    await navigator.clipboard.writeText(text);
    setNotice(successMessage);
  } catch {
    setNotice('复制失败，请手动选择文本。', true);
  }
}

function renderMetrics(totals) {
  const items = [
    ['已注册节点', totals.backends, 'Backend registry'],
    ['可用节点', totals.enabledBackends, 'Ready to execute'],
    ['排队任务', totals.queued, 'Pending jobs'],
    ['运行中', totals.active, 'Active workers'],
    ['定时工作流', totals.schedules, 'Scheduled tasks'],
  ];
  $('#totals').innerHTML = items
    .map(
      ([label, value, meta]) =>
        `<article class="metric-card"><span class="metric-label">${label}</span><strong class="metric-value">${value}</strong><span class="metric-meta">${meta}</span></article>`,
    )
    .join('');
}

function renderBackends(backends) {
  $('#backend-count').textContent =
    `${backends.length} ${backends.length === 1 ? 'node' : 'nodes'}`;
  $('#backends').innerHTML =
    backends
      .map(
        (backend) => `<tr>
          <td><div class="backend-name"><strong>${escape(backend.name)}</strong><small>${escape(backend.baseUrl)}</small></div></td>
          <td><code>${escape(backend.id)}</code></td>
          <td><span>${escape(backend.region ?? '未设置')}</span><br />${backend.tags.map((tag) => `<span class="tag">${escape(tag)}</span>`).join('') || '<span class="tag">无标签</span>'}</td>
          <td><span class="status ${backend.enabled ? 'succeeded' : 'idle'}">${backend.enabled ? '已启用' : '已停用'}</span></td>
          <td class="action-column"><div class="table-action"><button class="table-button" data-test="${escape(backend.id)}">测试</button><button class="table-button danger" data-delete="${escape(backend.id)}">删除</button></div></td>
        </tr>`,
      )
      .join('') ||
    '<tr><td class="empty-cell" colspan="5">还没有节点。使用上方安装向导接入第一台 VPS。</td></tr>';
  $('#task-backend').innerHTML =
    backends
      .filter((backend) => backend.enabled)
      .map(
        (backend) =>
          `<option value="${escape(backend.id)}">${escape(backend.name)} · ${escape(backend.id)}</option>`,
      )
      .join('') || '<option value="" disabled selected>请先添加可用 Backend</option>';
}

function renderTasks(tasks) {
  $('#tasks').innerHTML =
    tasks
      .map(
        (task) =>
          `<tr><td><code>${escape(task.id)}</code></td><td>${escape(task.backendId)}</td><td>${task.type === 'agent' ? 'Pi Agent' : 'Shell'}</td><td>${escape(task.summary ?? '—')}</td><td><span class="status ${escape(task.status)}">${statusLabel(task.status)}</span></td><td>${new Date(task.createdAt).toLocaleString('zh-CN')}</td></tr>`,
      )
      .join('') ||
    '<tr><td class="empty-cell" colspan="6">暂无任务。选择节点后即可创建第一个任务。</td></tr>';
}

async function refresh() {
  try {
    setNotice('正在同步实时状态…');
    const [dashboard, tasks] = await Promise.all([
      request('/api/dashboard'),
      request('/api/tasks?limit=25'),
    ]);
    renderMetrics(dashboard.totals);
    renderBackends(dashboard.backends);
    renderTasks(tasks);
    setNotice('控制平面已同步');
  } catch (error) {
    setNotice(`连接失败：${error.message}`, true);
  }
}

$('#backend-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  try {
    await request('/api/backends', {
      method: 'POST',
      body: JSON.stringify({
        id: form.get('id'),
        name: form.get('name'),
        baseUrl: form.get('baseUrl'),
        region: form.get('region') || undefined,
        tags: String(form.get('tags'))
          .split(',')
          .map((item) => item.trim())
          .filter(Boolean),
        enabled: true,
      }),
    });
    event.currentTarget.reset();
    await refresh();
    setNotice('Backend 已添加并验证连通性');
  } catch (error) {
    setNotice(`添加失败：${error.message}`, true);
  }
});

$('#task-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const type = form.get('type');
  try {
    await request('/api/tasks', {
      method: 'POST',
      body: JSON.stringify({
        backendId: form.get('backendId'),
        type,
        ...(type === 'shell' ? { command: form.get('content') } : { prompt: form.get('content') }),
        cwd: form.get('cwd'),
        profile: 'full',
        timeoutSeconds: Number(form.get('timeoutSeconds')),
      }),
    });
    event.currentTarget.reset();
    await refresh();
    setNotice('任务已进入节点队列');
  } catch (error) {
    setNotice(`创建任务失败：${error.message}`, true);
  }
});

$('#task-type').addEventListener('change', (event) => {
  $('#task-content-label').textContent = event.target.value === 'shell' ? '命令' : '任务目标';
});
$('#backends').addEventListener('click', async (event) => {
  const button = event.target.closest('button');
  if (!button) return;
  try {
    if (button.dataset.test) {
      await request(`/api/backends/${button.dataset.test}/test`, { method: 'POST' });
      setNotice('节点健康检查通过');
      await refresh();
    }
    if (button.dataset.delete && window.confirm(`删除 Backend ${button.dataset.delete}？`)) {
      await request(`/api/backends/${button.dataset.delete}`, { method: 'DELETE' });
      await refresh();
    }
  } catch (error) {
    setNotice(`操作失败：${error.message}`, true);
  }
});

$('#installer-form').addEventListener('input', updateInstallerCommand);
$('#installer-form').addEventListener('change', updateInstallerCommand);
$('#copy-install-command').addEventListener('click', () =>
  copyText($('#install-command').textContent, '安装命令已复制到剪贴板'),
);
$('#copy-mcp-url').addEventListener('click', () =>
  copyText($('#mcp-url').textContent, 'MCP 地址已复制到剪贴板'),
);
$('#refresh').addEventListener('click', refresh);
$('#mcp-url').textContent = `${origin}/mcp`;
updateInstallerCommand();
refresh();
