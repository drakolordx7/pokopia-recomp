import { getVersion } from '@tauri-apps/api/app';
import { esc, icon } from '../util.js';

export async function render(el, ctx) {
  let version = '';
  try {
    version = await getVersion();
  } catch {
    version = 'dev';
  }
  const g = ctx.game;
  el.innerHTML = `
    <div class="page">
      <div class="hero">
        <h1>About</h1>
        <p class="dim">Launcher for the ${esc(g.short_title)} PC port (a static recompilation, ${esc(g.exe)}).</p>
      </div>
      <div class="card">
        <h2>${icon('info')}${esc(g.launcher_name)}</h2>
        <dl class="kv">
          <dt>Version</dt><dd>${version}</dd>
          <dt>Licence</dt><dd>GPL-3.0-or-later</dd>
          <dt>Built with</dt><dd>Tauri 2, Lucide icons (ISC)</dd>
        </dl>
      </div>
      <div class="banner warn">
        ${icon('triangle-alert')}
        <div><strong>No game files included.</strong> You need your own legally dumped copy of the game. The launcher and the port contain no game data, and this project is not affiliated with or endorsed by ${esc(g.disclaimer)}.</div>
      </div>
    </div>`;
}
