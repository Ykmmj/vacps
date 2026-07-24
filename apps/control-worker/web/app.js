const notice = document.querySelector('#notice');
const request = async (url, init) => {
  const response = await fetch(url, { headers: { 'content-type': 'application/json' }, ...init });
  if (response.status === 204) return null;
  const body = await response.json();
  if (!response.ok) throw new Error(body.error?.message ?? `HTTP ${response.status}`);
  return body;
};
const setNotice = (message) => (notice.textContent = message);
const escape = (value) =>
  String(value).replace(
    /[&<>'"]/g,
    (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[char],
  );

async function refresh() {
  try {
    setNotice('正在刷新…');
    const [dashboard, tasks] = await Promise.all([
      request('/api/dashboard'),
      request('/api/tasks?limit=25'),
    ]);
    const totals = dashboard.totals;
    document.querySelector('#totals').innerHTML = [
      ['Backend', totals.backends],
      ['已启用', totals.enabledBackends],
      ['队列中', totals.queued],
      ['运行中', totals.active],
      ['定时任务', totals.schedules],
    ]
      .map(([name, value]) => `<div class="card">${name}<b>${value}</b></div>`)
      .join('');
    document.querySelector('#backends').innerHTML =
      dashboard.backends
        .map(
          (backend) => `<tr>
      <td>${escape(backend.name)}</td><td><code>${escape(backend.id)}</code></td><td>${escape(backend.region ?? '-')}</td><td>${escape(backend.tags.join(', '))}</td><td>${backend.enabled ? '是' : '否'}</td>
      <td><button class="secondary" data-test="${backend.id}">测试</button> <button class="danger" data-delete="${backend.id}">删除</button></td></tr>`,
        )
        .join('') || '<tr><td colspan="6">尚未添加 Backend</td></tr>';
    document.querySelector('#task-backend').innerHTML = dashboard.backends
      .filter((backend) => backend.enabled)
      .map(
        (backend) =>
          `<option value="${escape(backend.id)}">${escape(backend.name)} (${escape(backend.id)})</option>`,
      )
      .join('');
    document.querySelector('#tasks').innerHTML =
      tasks
        .map(
          (task) =>
            `<tr><td><code>${escape(task.id)}</code></td><td>${escape(task.backendId)}</td><td>${task.type}</td><td>${escape(task.summary ?? '')}</td><td><span class="status ${task.status}">${task.status}</span></td><td>${new Date(task.createdAt).toLocaleString()}</td></tr>`,
        )
        .join('') || '<tr><td colspan="6">暂无任务</td></tr>';
    setNotice('已刷新');
  } catch (error) {
    setNotice(`错误：${error.message}`);
  }
}

document.querySelector('#backend-form').addEventListener('submit', async (event) => {
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
          .map((x) => x.trim())
          .filter(Boolean),
        enabled: true,
      }),
    });
    event.currentTarget.reset();
    await refresh();
  } catch (error) {
    setNotice(`错误：${error.message}`);
  }
});
document.querySelector('#task-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const type = form.get('type');
  const content = String(form.get('content'));
  try {
    await request('/api/tasks', {
      method: 'POST',
      body: JSON.stringify({
        backendId: form.get('backendId'),
        type,
        ...(type === 'shell' ? { command: content } : { prompt: content }),
        cwd: form.get('cwd'),
        profile: 'full',
        timeoutSeconds: Number(form.get('timeoutSeconds')),
      }),
    });
    event.currentTarget.reset();
    await refresh();
  } catch (error) {
    setNotice(`错误：${error.message}`);
  }
});
document.querySelector('#task-type').addEventListener('change', (event) => {
  document.querySelector('#task-content-label').textContent =
    event.target.value === 'shell' ? '命令' : '任务目标';
});
document.querySelector('#backends').addEventListener('click', async (event) => {
  const button = event.target.closest('button');
  if (!button) return;
  try {
    if (button.dataset.test) {
      await request(`/api/backends/${button.dataset.test}/test`, { method: 'POST' });
      setNotice('连接正常');
    }
    if (button.dataset.delete && confirm(`删除 ${button.dataset.delete}？`)) {
      await request(`/api/backends/${button.dataset.delete}`, { method: 'DELETE' });
      await refresh();
    }
  } catch (error) {
    setNotice(`错误：${error.message}`);
  }
});
document.querySelector('#refresh').addEventListener('click', refresh);
refresh();
