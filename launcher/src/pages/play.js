import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

const hex = (c) => '0x' + (c >>> 0).toString(16).toUpperCase().padStart(8, '0');

function statusChip(s) {
  if (s.state === 'running') return `<span class="chip warn">${icon('loader-circle', 'spin')}Running</span>`;
  if (s.state === 'exited') {
    const code = s.code === null || s.code === undefined ? 'no exit code' : `code ${s.code} (${hex(s.code)})`;
    return s.crashed
      ? `<span class="chip danger">${icon('circle-alert')}Stopped abnormally, ${esc(code)}</span>`
      : `<span class="chip ok">${icon('circle-check')}Exited, ${esc(code)}</span>`;
  }
  return `<span class="chip">Ready</span>`;
}

function reportHtml(s, game) {
  if (s.state !== 'exited') return '';
  const files = s.files.filter((f) => s.crashed || f.name === game.log);
  return `
    <div class="card">
      <h2>${icon(s.crashed ? 'circle-alert' : 'file-text')}${s.crashed ? 'The game did not end cleanly' : 'Last run'}</h2>
      <p class="dim">${esc(s.message)}</p>
      ${files.length ? `<div class="row wrap" style="margin-top: var(--space-4)">${files
        .map((f) => `<button class="btn" data-open="${esc(f.name)}">${icon('file-text')}${esc(f.name)}${f.fresh && f.name !== game.log ? ' (new)' : ''}</button>`)
        .join('')}</div>` : ''}
    </div>`;
}

// The shader cache while it fills: setup's live progress when it is translating, else what the cache holds.
export function shaderPct(s) {
  const live = s.progress.state === 'running' && s.progress.phase === 'translate' && s.progress.total;
  const done = live ? s.progress.done : s.shaders_done;
  const total = live ? s.progress.total : s.shaders_total;
  return total ? Math.min(100, Math.floor((100 * done) / total)) : null;
}

export function shaderLine(s) {
  if (s.complete) return 'ready';
  const pct = shaderPct(s);
  const running = s.progress.state === 'running';
  const head = pct === null ? 'Shaders: being prepared' : `Shaders: ${pct}%`;
  return `${head} (the game can start; missing shaders are made while you play)${running ? '' : '. Set up continues them in the background.'}`;
}

export async function render(el, ctx) {
  const game = ctx.game;
  const [cfg, ov] = await Promise.all([api('get_config'), api('get_overview')]);
  let setup = await api('setup_status');
  const report = cfg.dump_dir ? await api('validate_dump', { path: cfg.dump_dir }) : null;
  const chosen = report && report.chosen;
  let status = await api('game_status');

  function draw() {
    const running = status.state === 'running';
    const problems = [];
    if (!cfg.dump_dir) problems.push('No game files chosen yet.');
    else if (!chosen) problems.push(`The game files at ${cfg.dump_dir} are not usable: ${report.errors.join(' ')}`);
    if (!ov.exe_exists) problems.push(`${game.exe} was not found in ${ov.game_dir}. Put the launcher next to ${game.exe}, or set the game folder in Settings.`);
    if (chosen && ov.exe_exists && !setup.playable) problems.push(setup.progress.state === 'running' ? 'Setup is still building the game (Game files).' : 'Setup is not finished yet: open Game files and press Set up.');
    const ready = problems.length === 0;

    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${esc(game.short_title)}</h1>
          <p class="dim">Native PC port. Your game files stay where they are.</p>
        </div>

        ${problems.map((p) => `<div class="banner danger">${icon('circle-x')}<div>${esc(p)}</div></div>`).join('')}
        ${problems.length && (!chosen || !setup.playable) ? `<div><button class="btn primary" id="to-setup">${icon(chosen ? 'wrench' : 'folder-open')}${chosen ? 'Set up' : 'Choose game files'}</button></div>` : ''}

        <div class="row wrap">
          <button class="btn primary big" id="play" ${ready && !running ? '' : 'disabled'}>${icon('play')}${running ? 'Running' : 'Play'}</button>
          ${statusChip(status)}
        </div>
        ${ready && !setup.complete ? `<p class="dim" id="shader-line">${icon('loader-circle', setup.progress.state === 'running' ? 'spin' : '')}${esc(shaderLine(setup))}</p>` : ''}

        ${reportHtml(status, game)}

        <div class="card">
          <h2>${icon('hard-drive')}Setup</h2>
          <dl class="kv">
            <dt>Game files</dt>
            <dd>${chosen ? `${chosen.kind === 'base' ? 'Base game' : 'Update ' + esc(chosen.version)}<div class="path">${esc(chosen.path)}</div>` : '<span class="faint">not set</span>'}</dd>
            <dt>Shaders</dt>
            <dd>${setup.complete ? 'ready' : setup.shaders_total || setup.progress.state === 'running' ? esc(shaderLine(setup)) : '<span class="faint">not set up</span>'}</dd>
            <dt>Game folder</dt>
            <dd>${ov.exe_exists ? `${esc(game.exe)} found` : `<span class="faint">${esc(game.exe)} missing</span>`}<div class="path">${esc(ov.game_dir)}</div></dd>
          </dl>
          <div class="row" style="margin-top: var(--space-4)">
            <button class="btn" id="open-dir">${icon('folder-open')}Open game folder</button>
            <button class="btn quiet" id="change">Change game files</button>
          </div>
        </div>
      </div>`;
    refreshIcons();

    el.querySelector('#play')?.addEventListener('click', async () => {
      try {
        await api('play');
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#to-setup')?.addEventListener('click', () => ctx.go('setup'));
    el.querySelector('#change')?.addEventListener('click', () => ctx.go('setup'));
    el.querySelector('#open-dir')?.addEventListener('click', () => api('open_game_dir').catch((e) => toast(errText(e), 'error')));
    el.querySelectorAll('[data-open]').forEach((b) =>
      b.addEventListener('click', () => api('open_game_file', { name: b.dataset.open }).catch((e) => toast(errText(e), 'error'))),
    );
  }

  ctx.onStatus((s) => {
    status = s;
    draw();
  });
  // setup keeps translating shaders in the background: redraw when the percentage or the state changes
  let shown = `${shaderPct(setup)}|${setup.progress.state}`;
  ctx.onSetup(async (p) => {
    setup = p.state === 'running' && p.phase !== 'check' ? { ...setup, progress: p } : await api('setup_status');
    const now = `${shaderPct(setup)}|${setup.progress.state}`;
    if (now === shown) return;
    shown = now;
    draw();
  });
  draw();
}
