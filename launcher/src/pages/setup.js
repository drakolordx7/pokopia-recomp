import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';
import { shaderLine } from './play.js';

let GAME = null; // the game manifest (ctx.game), set by render()

// one listener for the whole session; the page being shown sets the handler
let dlssHandler = null;
listen('dlss-progress', (ev) => dlssHandler && dlssHandler(ev.payload));

function candidateHtml(c, chosen) {
  const title = c.kind === 'base' ? 'Base game' : `Update ${esc(c.version)}`;
  const used = chosen && chosen.path === c.path;
  return `
    <div class="candidate ${used ? 'chosen' : ''}">
      <div class="row between">
        <h3>${title}</h3>
        ${used ? `<span class="chip ok">${icon('circle-check')}Will be used</span>` : c.ok ? '<span class="chip">Valid, older</span>' : `<span class="chip danger">${icon('circle-x')}Incomplete</span>`}
      </div>
      <div class="path">${esc(c.path)}</div>
      <ul class="checks">
        ${c.checks.map((k) => `<li class="${k.ok ? 'ok' : 'bad'}">${icon(k.ok ? 'circle-check' : 'circle-x')}${esc(k.label)}</li>`).join('')}
      </ul>
    </div>`;
}

// Raw dump mode: NSP / XCI files plus the player's keys, extracted by Set up into <game folder>/dump.
function rawHtml(raw) {
  const list = (items, empty) =>
    items.length
      ? `<ul class="checks">${items.map((f, i) => `<li class="ok">${icon('file-text')}${esc(f)} <button class="btn quiet" data-del="${i}">Remove</button></li>`).join('')}</ul>`
      : `<p class="faint">${empty}</p>`;
  const keyList = raw.keys.length
    ? `<ul class="checks">${raw.keys.map((f) => `<li class="ok">${icon('shield-check')}${esc(f)}</li>`).join('')}</ul>`
    : '<p class="faint">No keys file chosen.</p>';
  const f = raw.found;
  return `
    <div class="card" id="raw-card">
      <h2>${icon('hard-drive')}Or: dump files (NSP / XCI)</h2>
      <div class="stack">
        <p class="dim">Pick the game's <code>.nsp</code> or <code>.xci</code> and, if the game needs it, its update <code>.nsp</code>, plus the <code>prod.keys</code> (and <code>title.keys</code>, if you have one) dumped from your own console. Set up decrypts and extracts them into <code>dump/</code> in the game folder (about as much space as the game) and then carries on as with a dump folder. The keys files are only read by the extractor; nothing copies, uploads or logs them.</p>
        ${list(raw.inputs, 'No dump files chosen.')}
        <div class="row">
          <button class="btn" id="raw-add">${icon('folder-open')}Add dump files</button>
          <button class="btn" id="raw-keys">${icon('shield-check')}Choose keys</button>
          ${raw.inputs.length || raw.keys.length ? '<button class="btn quiet" id="raw-clear">Clear</button>' : ''}
        </div>
        ${keyList}
        ${raw.error ? `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(raw.error)}</div></div>` : ''}
        ${f ? `<p class="dim">${icon('circle-check')} Found ${f.dir === 'base' ? 'the base game' : `update ${esc(f.display_version || String(f.version))}`} (program build ${esc(f.build_id.slice(0, 16).toLowerCase())}..., ${f.files} data files, ${(f.bytes / 1e9).toFixed(1)} GB)${f.zbic_modules ? `, ${f.zbic_modules} ZBIC module(s)` : ''}.</p>` : ''}
        ${f && f.notes.length ? `<ul class="checks">${f.notes.map((n) => `<li>${icon('circle-alert')}${esc(n)}</li>`).join('')}</ul>` : ''}
        <div class="row">
          <button class="btn ${raw.saved ? '' : 'primary'}" id="raw-use" ${raw.inputs.length && raw.keys.length && !raw.busy ? '' : 'disabled'}>${icon('circle-check')}${raw.busy ? 'Checking...' : 'Use these dump files'}</button>
          ${raw.saved ? `<span class="chip ok">${icon('circle-check')}In use</span>` : ''}
        </div>
      </div>
    </div>`;
}

function shadersHtml(s, saved) {
  const p = s.progress;
  const running = p.state === 'running';
  const pct = p.total ? Math.floor((100 * p.done) / p.total) : 0;
  let body = '';
  if (running) {
    body = `<p class="dim">${esc(p.message)}</p>
      <progress max="100" value="${p.total ? pct : ''}"></progress>
      <p class="faint">${p.total ? `${pct}%. ` : ''}You can pause and continue later; finished work is kept.</p>
      ${s.playable ? `<p class="dim">${icon('circle-check')} ${esc(shaderLine(s))}</p>` : ''}`;
  } else if (p.state === 'error') {
    body = `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(p.message)}</div></div>`;
  } else if (s.complete) {
    body = `<p class="dim">${icon('circle-check')} Done. ${esc(GAME.exe)} is built and the game's shaders are in <code>shader_cache/spirv</code> next to it.</p>`;
  } else {
    body = `<p class="dim">${p.state === 'cancelled' ? esc(p.message) : `Checks your game files, builds ${esc(GAME.exe)} from them on this PC (the download contains no game code; the compile takes a while and needs a few GB free while it runs), then translates the game's shaders from your romfs (about 100,000 shaders; needs about 36 GB free while it runs and keeps about 26 GB). You can play as soon as ${esc(GAME.exe)} is built: the shaders keep being translated in the background, and any the game needs first are made while you play. You can pause and continue later.`}</p>`;
  }
  const label = s.complete ? 'Set up again' : s.started ? 'Continue setup' : 'Set up';
  return `
    <div class="card">
      <h2>${icon('wrench')}Set up</h2>
      <div class="stack">
        ${body}
        <div class="row">
          ${running
            ? `<button class="btn" id="setup-cancel">${icon('circle-x')}Pause</button>`
            : `<button class="btn ${s.complete ? '' : 'primary'}" id="setup-run" ${saved ? '' : 'disabled'}>${icon('wrench')}${label}</button>`}
          ${(s.complete && !running) || (s.playable && running) ? `<button class="btn primary" id="to-play">${icon('play')}Go to Play</button>` : ''}
        </div>
      </div>
    </div>`;
}

function dlssHtml(d, busy, p) {
  if (!d.available) return '';
  const pct = p && p.total ? Math.min(100, Math.floor((100 * p.done) / p.total)) : 0;
  return `
    <div class="card" id="dlss-card">
      <h2>${icon('monitor')}NVIDIA DLSS (optional)</h2>
      <div class="stack">
        <p class="dim">FSR 3 is included and needs nothing more. DLSS and DLAA need an NVIDIA RTX GPU and one more file, <code>nvngx_dlss.dll</code>, which NVIDIA licenses separately, so it is not in the download. This button gets it from NVIDIA's own DLSS repository on GitHub (version 310.9.16, checked against a fixed hash) and puts it in <code>${esc(d.dir)}</code>. By downloading it you accept the NVIDIA RTX SDKs licence: <code>licenses/LICENSE.NVIDIA-RTX-SDKs.txt</code>, also at github.com/NVIDIA/DLSS.</p>
        ${busy ? `<p class="dim">${esc(p?.message || 'Starting...')}</p><progress max="100" value="${pct}"></progress>` : ''}
        <div class="row">
          ${d.present ? `<span class="chip ok">${icon('circle-check')}nvngx_dlss.dll is installed</span>` : ''}
          <button class="btn ${d.present ? '' : 'primary'}" id="dlss-get" ${busy ? 'disabled' : ''}>${icon('monitor')}${d.present ? 'Download again' : 'Download DLSS from NVIDIA'}</button>
        </div>
      </div>
    </div>`;
}

export async function render(el, ctx) {
  GAME = ctx.game;
  const cfg = await api('get_config');
  let path = cfg.dump_dir || '';
  let saved = path;
  let report = path ? await api('validate_dump', { path }) : null;
  let setup = await api('setup_status');
  let dlss = await api('dlss_status');
  let dlssBusy = false;
  let dlssProgress = null;
  const raw = { inputs: [...(cfg.raw_inputs || [])], keys: [...(cfg.key_files || [])], found: null, error: '', busy: false, saved: (cfg.raw_inputs || []).length > 0 };

  function draw() {
    const ok = report && report.chosen;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${ctx.firstRun ? 'Welcome. Set up your game files.' : 'Game files'}</h1>
          <p class="dim">${esc(GAME.launcher_name)} does not include the game. You need your own legally dumped copy: a decrypted dump folder, or your NSP / XCI files with your own keys. The launcher only reads them and never writes to them.</p>
        </div>

        <div class="card">
          <h2>${icon('folder-search')}Dump folder</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="C:/path/to/${esc(GAME.short_title)}_dump" value="${esc(path)}" />
              <button class="btn" id="browse">${icon('folder-open')}Browse</button>
            </div>
            <p class="faint">Expected layout: <code>base/</code> and <code>updates/&lt;version&gt;/</code>, each with <code>exefs/</code> (${esc(GAME.exefs_files.join(', '))}) and <code>romfs/</code>. The highest valid update is used, otherwise the base game.</p>
          </div>
        </div>

        ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map(esc).join('<br>')}</div></div>` : ''}
        ${report && report.candidates.length ? `<div class="stack">${report.candidates.map((c) => candidateHtml(c, report.chosen)).join('')}</div>` : ''}

        <div class="row">
          <button class="btn ${ok && saved !== path ? 'primary' : ''}" id="use" ${ok && setup.progress.state !== 'running' ? '' : 'disabled'}>${icon('circle-check')}Use these game files</button>
          ${ctx.firstRun ? '' : '<button class="btn quiet" id="cancel">Back</button>'}
        </div>

        ${rawHtml(raw)}

        ${shadersHtml(setup, raw.saved || (ok && saved === path && !(cfg.raw_inputs || []).length))}

        ${dlssHtml(dlss, dlssBusy, dlssProgress)}
      </div>`;
    refreshIcons();

    const input = el.querySelector('#path');
    const check = async () => {
      path = input.value.trim();
      report = path ? await api('validate_dump', { path }) : null;
      draw();
    };
    input.addEventListener('change', check);
    input.addEventListener('keydown', (e) => e.key === 'Enter' && check());
    el.querySelector('#browse').addEventListener('click', async () => {
      const picked = await api('pick_folder', { title: `Choose your decrypted ${GAME.short_title} dump folder`, start: input.value || null });
      if (picked) {
        input.value = picked;
        check();
      }
    });
    el.querySelector('#use').addEventListener('click', async () => {
      try {
        await api('save_dump', { path });
        saved = path;
        cfg.raw_inputs = [];
        cfg.key_files = [];
        raw.saved = false;
        toast('Game files saved. Next: Set up.', 'ok');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    const rawChanged = () => {
      raw.saved = false;
      raw.found = null;
      raw.error = '';
      draw();
    };
    el.querySelector('#raw-add').addEventListener('click', async () => {
      const files = await api('pick_files', { title: 'Choose your dump files (base game and update)', filterName: 'Switch dumps', extensions: ['nsp', 'xci', 'nsz', 'xcz'], multiple: true });
      for (const f of files) if (!raw.inputs.includes(f)) raw.inputs.push(f);
      if (files.length) rawChanged();
    });
    el.querySelector('#raw-keys').addEventListener('click', async () => {
      const files = await api('pick_files', { title: 'Choose prod.keys (and title.keys, if you have one)', filterName: 'Keys', extensions: ['keys', 'txt'], multiple: true });
      if (files.length) {
        // prod.keys first: the extractor reads them in order
        raw.keys = files.sort((a, b) => Number(/title\.keys$/i.test(a)) - Number(/title\.keys$/i.test(b)));
        rawChanged();
      }
    });
    el.querySelector('#raw-clear')?.addEventListener('click', () => {
      raw.inputs = [];
      raw.keys = [];
      rawChanged();
    });
    el.querySelectorAll('[data-del]').forEach((b) =>
      b.addEventListener('click', () => {
        raw.inputs.splice(Number(b.dataset.del), 1);
        rawChanged();
      }),
    );
    el.querySelector('#raw-use').addEventListener('click', async () => {
      raw.busy = true;
      raw.error = '';
      draw();
      try {
        raw.found = await api('save_raw', { inputs: raw.inputs, keys: raw.keys });
        raw.saved = true;
        cfg.raw_inputs = [...raw.inputs];
        cfg.key_files = [...raw.keys];
        toast('Dump files saved. Next: Set up.', 'ok');
      } catch (e) {
        raw.error = errText(e);
      }
      raw.busy = false;
      draw();
    });
    el.querySelector('#cancel')?.addEventListener('click', () => ctx.go('play'));
    el.querySelector('#to-play')?.addEventListener('click', () => ctx.go('play'));
    el.querySelector('#setup-run')?.addEventListener('click', async () => {
      try {
        await api('start_setup');
        setup = await api('setup_status');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#setup-cancel')?.addEventListener('click', () => api('cancel_setup'));
    el.querySelector('#dlss-get')?.addEventListener('click', async () => {
      dlssBusy = true;
      dlssProgress = null;
      draw();
      try {
        await api('fetch_dlss');
        toast('nvngx_dlss.dll downloaded and verified. Restart the game to use DLSS.', 'ok');
      } catch (e) {
        toast(errText(e), 'error');
      }
      dlssBusy = false;
      dlss = await api('dlss_status');
      draw();
    });
  }

  dlssHandler = (p) => {
    dlssProgress = p;
    const card = el.querySelector('#dlss-card');
    if (card && dlssBusy) {
      card.outerHTML = dlssHtml(dlss, true, p);
      refreshIcons();
    }
  };

  ctx.onSetup(async (p) => {
    const was = setup.progress.state;
    // a new phase (the game build finished, shaders started): fetch the whole status, Play may have become possible
    setup = p.state === 'running' && p.phase === setup.progress.phase ? { ...setup, progress: p } : await api('setup_status');
    // progress ticks only touch the setup card's text, so the path field keeps its focus
    if (was === 'running' && p.state === 'running' && el.querySelector('progress')) {
      const card = el.querySelector('#setup-cancel')?.closest('.card');
      if (card) {
        card.outerHTML = shadersHtml(setup, true);
        refreshIcons();
        el.querySelector('#setup-cancel')?.addEventListener('click', () => api('cancel_setup'));
        el.querySelector('#to-play')?.addEventListener('click', () => ctx.go('play'));
        return;
      }
    }
    draw();
  });
  draw();
}
