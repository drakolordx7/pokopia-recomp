# Plan de adaptación a Steam Deck

Estado: propuesta (nada de lo descrito está implementado todavía). Rama de trabajo: `claude/steam-deck-adaptation-5d8v0x`.

## 1. Diagnóstico

### De dónde sale el error de webview

`PokopiaLauncher.exe` es una app Tauri 2 (`launcher/src-tauri`). En Windows, Tauri dibuja la interfaz con **Microsoft Edge WebView2**,
un runtime del sistema que no va dentro del exe (`tauri.conf.json` no activa `bundle`, y aunque lo hiciera Tauri solo empaqueta un
*bootstrapper* que descarga WebView2 de Microsoft). En un prefijo de Proton no existe WebView2, y Chromium/WebView2 no funciona de
forma fiable bajo Wine (instalador que falla, ventanas en blanco o cierres; Tauri lo da por no soportado). Por eso el lanzador muere
antes de hacer nada.

Conclusión: **empaquetar el runtime de WebView2 no es una solución viable**. Lo que hay que cambiar es la capa de interfaz del
lanzador, no sus dependencias.

### Qué dependencias ya están resueltas

| Pieza | Cómo se resuelve hoy | Bajo Proton |
|---|---|---|
| CRT de MSVC | estático (`/MT`, `CMAKE_MSVC_RUNTIME_LIBRARY`) | sin problema |
| SDL3 | estático en `pkp_rt.dll` | usa XInput/RawInput de Wine, Steam Input lo cubre |
| Vulkan 1.3 | `vulkan-1.dll` del sistema | winevulkan sobre RADV, cumple 1.3 + dynamicRendering + synchronization2 |
| XAudio2 | `xaudio2.lib` | FAudio de Proton |
| Media Foundation (vídeos, `movie_mf.cpp`) | MF del sistema | la implementación MF de Proton sobre GStreamer, hay que probarla |
| Descodificadores ASTC, fuentes, iconos, FSR 3, Streamline | DLLs y ficheros junto al exe | DLLs Windows normales, FSR usa Vulkan |
| DLSS | descarga opcional | solo NVIDIA, no aplica en Deck |

Es decir, el único bloqueo conocido es WebView2. Todo lo demás es trabajo de verificación y ajuste, no de empaquetado.

### Qué hace realmente el lanzador

Las cuatro cosas que hace no necesitan un navegador:

1. Escribir `pkp.ini` (`settings.rs`, `ini.rs`, `dump.rs`).
2. Construir `pkp.exe` en el PC del jugador: `tools/build/pkp_build.py` con el Python y el Zig empaquetados (`gamebuild.rs`).
3. Construir la caché de shaders: `extract_corpus.py` + `spirv_harness.exe` (.NET autocontenido) (`shaders.rs`).
4. Lanzar `pkp.exe --log pkp.log` desde su carpeta (`play.rs`; `release/Play.bat` hace lo mismo sin lanzador).

Esos módulos ya están escritos sin dependencias de Tauri (`examples/first_run.rs` los compila solos y ejecuta la primera configuración
sin ventana). Reutilizarlos es la base del plan.

### Dos hechos que simplifican todo

- **La carpeta del juego es portable.** `pkp.exe`, `pkp_rt.dll` y `shader_cache/spirv` (SPIR-V + `results.tsv`) se derivan del dump,
  no de la GPU ni del PC. Una carpeta montada en un PC Windows funciona copiada a la Deck. Esto da una vía inmediata de prueba.
- **Las herramientas de la primera configuración son multiplataforma.** Python, Zig (que ya compila para `x86_64-windows-gnu`, un
  objetivo cruzado desde cualquier SO) y .NET existen para Linux. Solo `pkp_rt.dll` y el `pkp.exe` resultante son binarios Windows.
  Eso abre una vía "herramientas nativas en SteamOS + juego bajo Proton", mucho más rápida que correr Zig y .NET dentro de Wine.

### Lo que queda fuera

Un port nativo del runtime a Linux. `runtime/CMakeLists.txt` aborta fuera de WIN32; 63 ficheros incluyen `windows.h`; el espacio de
direcciones del invitado se monta con `CreateFileMapping` + `MapViewOfFileEx` en direcciones fijas; un hilo Win32 por hilo invitado;
XAudio2; Media Foundation; DXGI para HDR. Son meses de trabajo y no es lo que se pide.

## 2. Plan por fases

### Fase 0: validar `pkp.exe` bajo Proton (1-2 días, sin código)

Antes de tocar el lanzador hay que saber si el juego en sí arranca en la Deck. Nadie lo ha visto todavía porque el lanzador falla antes.

1. Hacer la configuración completa en un PC Windows (lanzador actual). Copiar la carpeta del juego entera (con `shader_cache/spirv`,
   `fonts/`, `assets/`, `upscalers/`, los `astcdec-*.dll`) y el dump a la Deck, mejor a la tarjeta SD.
2. En `pkp.ini`, poner rutas que vea Proton (`Z:/run/media/deck/...`) en `[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, y
   `[display] window_mode = borderless`.
3. Añadir `pkp.exe` como juego no Steam, forzar Proton (Experimental o GE), opciones de lanzamiento `--log pkp.log`.
4. Leer `pkp.log` y, si los hay, `pkp_crash.txt` / `pkp_fatal.txt`. Qué mirar:
   - La línea `GPU 0: AMD Custom GPU 0405 ... Vulkan 1.3.x` sin `NOT USABLE`.
   - Que la reserva del espacio invitado (`0x71_0000_0000` en adelante, heap de hasta 8 GiB con `SEC_RESERVE`) no falle. En Linux
     x86-64 esas direcciones están libres (mmap trabaja cerca de `0x7Fxx_xxxx_xxxx`), pero hay que verlo en el log.
   - Audio: `xaudio2: output device (default)`.
   - Vídeos: si Media Foundation de Proton no descodifica el MP4, ver si el juego sigue o se cuelga.
   - Pico de memoria del `memguard` (`[memory] limit_gb`, por defecto 16 en Switch 2). La Deck tiene 16 GB compartidos con la GPU.
   - Rendimiento a 1280x800 (`aspect = auto` ya hace Hor+ para 16:10) con `quality_preset = switch2` y FSR 3.
5. Anotar cada fallo. El resultado decide el contenido de la fase 3.

Entregable: `docs/STEAM_DECK.md` con el procedimiento manual y las conclusiones. Mientras no exista otra cosa, esta es la forma de
jugar en la Deck.

### Fase 1: lanzador sin ventana, modo línea de comandos (2-3 días)

Objetivo: que la release actual funcione en la Deck sin WebView2, con el mínimo cambio.

- `launcher/src-tauri/src/main.rs`: si hay argumentos, no se construye el `tauri::Builder` (y por tanto no se crea ningún webview);
  se entra en un `cli.rs` nuevo basado en `examples/first_run.rs`:
  - `PokopiaLauncher.exe --setup <carpeta del dump> [--jobs N]`: `dump::scan`, `settings::ensure_play_paths`, `gamebuild::run`,
    `shaders::run`. Progreso por consola (`AttachConsole(ATTACH_PARENT_PROCESS)`, el exe es subsistema GUI) y a `setup.log`.
    Reanudable como hoy (los marcadores `build/done.json`, `shader_cache/setup.json`, `results.tsv` ya lo permiten).
  - `PokopiaLauncher.exe --play`: lo mismo que el botón Play.
  - `PokopiaLauncher.exe --status`: estado de la configuración, en texto.
- `launcher/release/Setup.bat` junto a `Play.bat`.
- Guardar `launcher.json` junto al exe cuando se ejecuta en modo portable, en vez de en `AppData` del prefijo (se pierde si se borra el prefijo).
- Documentar en `docs/STEAM_DECK.md`: añadir `PokopiaLauncher.exe` a Steam con `--setup Z:\...` como opciones de lanzamiento, luego
  cambiar a `pkp.exe --log pkp.log` (o `--play`).

Con esto se verifica además que Python, Zig y `spirv_harness` (.NET 10 autocontenido, `InvariantGlobalization` ya activado) funcionan
bajo Wine. Tiempos estimados en la Deck bajo Proton: compilación de `pkp.exe` 20-40 min (7 min en un PC de escritorio), traducción de
shaders 4-8 h (1-2 h en PC). Son estimaciones; la fase 0 las afina. Esta lentitud es la razón de la fase 4.

### Fase 2: lanzador nativo sin webview (1-2 semanas)

Objetivo: una interfaz gráfica que funcione en Windows y bajo Proton, usable con mando, sin WebView2.

- Extraer la lógica a un crate de biblioteca `launcher/core` (`dump`, `ini`, `settings`, `gamebuild`, `shaders`, `dlss`, `play`).
  `play.rs` depende hoy de `tauri::AppHandle` para emitir eventos; se sustituye por un callback o un canal.
- Nuevo binario `launcher/native` con **egui/eframe** (backend `glow` u `wgpu`): OpenGL y Vulkan funcionan bajo Wine y gamescope.
  Cuatro páginas, las mismas que `launcher/src/pages/*.js` (setup, play, settings, about). Maquetado para 1280x800, fuentes grandes,
  navegación con mando (`gilrs` o SDL) además de ratón.
  - Alternativa: un lanzador Win32 clásico como `runtime/apps/launcher` (el de MPR, controles comunes, funciona perfecto en Wine).
    Desventaja: duplicar en C++ la lógica de configuración que ya existe en Rust. Por eso se recomienda egui.
- El selector de carpetas: `rfd` (usa el diálogo de Win32, que Wine implementa) más un campo de texto editable, porque en la Deck
  el diálogo es incómodo con mando.
- La release pasa a llevar el lanzador nativo como `PokopiaLauncher.exe`. Tauri se retira o se mantiene solo mientras dure la transición;
  mantener dos lanzadores a largo plazo no tiene sentido.
- Detección de entorno para los valores por defecto: variables `SteamDeck=1` y `STEAM_COMPAT_DATA_PATH`, y en proceso la exportación
  `wine_get_version` de `ntdll.dll`.

### Fase 3: ajustes del runtime para Proton y Deck (1-2 semanas, según la fase 0)

Lista inicial; la fase 0 la recorta o la amplía.

- `runtime/src/gfx/platform.cpp`: bajo Wine, tratar `window_mode = exclusive` como `borderless` desde el principio (gamescope no da
  modo exclusivo) y registrar en el log la versión de Wine/Proton.
- `runtime/src/gfx/extras/win_display.cpp`: comprobar que la consulta HDR (CCD + DXGI 1.6) falla limpiamente con el `dxgi.dll` de
  Proton y devuelve "sin HDR".
- Streamline/DLSS: no cargar `sl.interposer.dll` en GPUs que no sean NVIDIA ni bajo Wine; FSR 3 es el escalador de la Deck.
- Planificación: `SetPriorityClass`, `SetThreadAffinityMask`, `AvSetMmThreadCharacteristics` y `timeBeginPeriod` se ignoran en gran
  parte bajo Wine. Si la fase 0 muestra tirones o problemas de audio, revisar `sched.cpp`, `starvation_guard.cpp` y
  `audren/Integration/ThreadBoost.h` con un camino alternativo cuando se detecta Wine.
- Memoria: medir el pico real. Si pasa de unos 12 GB, documentar swap (16 GB, `swappiness` bajo) y buscar qué cachés del runtime se
  pueden limitar en la Deck.
- Vídeos: si MF de Proton no los descodifica, opción de saltarlos o ruta alternativa con el descodificador libvpx que ya se enlaza.
- Preset Deck en la configuración: `borderless`, `aspect = auto`, `quality_preset = switch2`, límite de fps 60 o 40, FSR 3 en
  calidad, `[fs] save_dir` junto al exe. Un botón "Steam Deck" en el lanzador o un `pkp.deck.ini` en `game-build/release_b`.
- Entrada: la Deck llega como mando Xbox por Steam Input, suficiente para SDL3. El modo ratón de Joy-Con 2 (botón central) se mapea a
  un clic del trackpad; giroscopio a ratón desde Steam Input. Publicar una configuración recomendada de Steam Input.
- Rutas: mantener rutas ASCII (`/run/media/deck/...`) hasta confirmar que Wine respeta `activeCodePage = UTF-8` del manifiesto.

### Fase 4 (opcional): herramientas nativas en SteamOS (2-3 semanas)

Objetivo: que la primera configuración corra a velocidad nativa en la Deck y no dependa de Wine para Zig, Python y .NET. El juego sigue
corriendo bajo Proton.

- `game-build/release_b/pkp_build.py`: `ZIG` según `sys.platform` (`zig` sin `.exe`), `creationflags` solo en Windows. El objetivo de
  compilación ya es `x86_64-windows-gnu`, así que Zig para Linux produce el mismo `pkp.exe`; `pkp_rt.lib` es una biblioteca de
  importación que el `lld` de Zig ya consume hoy.
- `tools/shaders/harness`: `dotnet publish -r linux-x64 --self-contained` a `tools/spirv_harness/spirv_harness`; `shaders::find_tools`
  acepta ambos nombres.
- Python: usar el `python3` de SteamOS con las ruedas `lz4` y `capstone` incluidas en `tools/python/`, o un Python autocontenido
  (python-build-standalone).
- Lanzador: el mismo crate egui compilado para `x86_64-unknown-linux-gnu`. Diferencias: escribir en `pkp.ini` rutas en forma
  `Z:/...` (`settings.rs`), y "Play" lanza `pkp.exe` a través de Proton (`proton run` con `STEAM_COMPAT_DATA_PATH` y
  `STEAM_COMPAT_CLIENT_INSTALL_PATH`, o `steam://rungameid/<id>` si el usuario ya lo añadió a Steam). Al principio basta con instrucciones
  claras para añadir `pkp.exe` a Steam; escribir `shortcuts.vdf` es frágil.
- Paquete `pokopia-<versión>-steamdeck.tar.gz`: herramientas Linux + el mismo runtime Windows (`pkp_rt.dll`, `sdk/`, `upscalers/`,
  `assets/`, `fonts/`). Un trabajo de CI en Ubuntu construye las herramientas; el runtime se sigue construyendo en Windows.

Tiempos estimados nativos en la Deck: compilación 10-20 min, shaders 2-4 h (frente a 20-40 min y 4-8 h bajo Wine).

## 3. Requisitos de hardware que hay que decir al usuario

- Disco: caché de shaders 26 GB, más 10 GB temporales del corpus durante la configuración, más los intermedios de la compilación (se borran
  al terminar), más el dump. Hacen falta unos 45 GB libres además del dump. En una Deck de 64 GB solo cabe en tarjeta SD.
- RAM: el README pide 16 GB, que es lo que tiene la Deck compartido con la GPU. Si el pico medido en la fase 0 se acerca, hace falta swap.
- La configuración completa en la Deck tarda horas. La alternativa "configurar en PC y copiar la carpeta" debe estar documentada siempre.

## 4. Riesgos

| Riesgo | Probabilidad | Mitigación |
|---|---|---|
| `pkp.exe` no reserva el espacio invitado bajo Wine | baja | fase 0 lo detecta en `pkp.log`; el código ya registra errores de `VirtualAlloc`/`MapViewOfFileEx` |
| Media Foundation de Proton no descodifica los vídeos | media | saltar vídeos o ruta libvpx (fase 3) |
| Memoria insuficiente en la Deck | media | medir, swap, limitar cachés |
| Zig o .NET fallan bajo Wine | baja-media | fase 1 lo prueba; la fase 4 los saca de Wine |
| Rendimiento insuficiente en RDNA2 a 800p | media | preset `switch2`, FSR 3, `render_scale`, límite 40 fps |
| Prioridades de hilos ignoradas por Wine afectan al audio o al pacing | media | camino alternativo detectando Wine |

## 5. Orden recomendado

1. Fase 0 ya (coste casi nulo, decide todo lo demás).
2. Fase 1 en paralelo: desbloquea la release actual en la Deck con una tarde de trabajo.
3. Fase 3 con lo que salga de la fase 0.
4. Fase 2 cuando se quiera una experiencia de usuario decente con mando.
5. Fase 4 solo si los tiempos bajo Wine resultan inaceptables.

## 6. Legal

Nada cambia: el paquete sigue sin contener código ni datos del juego. La caché de shaders y `pkp.exe` se derivan del dump del propio usuario
en su propio dispositivo (PC o Deck) y no se distribuyen.
