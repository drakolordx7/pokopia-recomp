# Pokopia en Steam Deck (fase 1: configuración sin lanzador)

Estado: primera versión probable. El juego (`pkp.exe`) corre bajo Proton; lo que cambia respecto a Windows es que la primera configuración
y el arranque se hacen con `pokopia-setup.exe`, una herramienta de consola, porque el lanzador gráfico (`PokopiaLauncher.exe`) necesita
WebView2 y eso no existe bajo Proton. El plan completo está en [STEAM_DECK_PLAN.md](STEAM_DECK_PLAN.md).

## Qué necesitas

- Una Steam Deck con **Proton Experimental** instalado (Biblioteca, Herramientas) o GE-Proton.
- Tu propio dump descifrado de la versión 2.0.1a (`<dump>/base/{exefs,romfs}` y/o `<dump>/updates/<versión>/{exefs,romfs}`).
- El paquete de la release (la carpeta con `PokopiaLauncher.exe`, `pkp_rt.dll`, `tools/`, `upscalers/`, `assets/`, `fonts/`).
- `pokopia-setup.exe` y `pokopia-deck.sh` copiados a esa misma carpeta. Salen del artefacto `pokopia-setup-windows-x64` del workflow
  *pokopia-setup* de GitHub Actions (pestaña Actions del repositorio) o de compilar `launcher/cli` (ver abajo).
- Espacio: unos 45 GB libres además del dump si configuras en la Deck (caché de shaders 26 GB más 10 GB temporales). En una Deck de 64 GB
  hace falta tarjeta SD. Si configuras en un PC y copias la carpeta, bastan los 26 GB de la caché.

## Opción A: configurar en un PC Windows y copiar (la más rápida)

1. En el PC, haz la configuración normal con `PokopiaLauncher.exe` (o `Setup.bat <dump>`).
2. Copia a la Deck (tarjeta SD) la carpeta del juego completa, con `pkp.exe`, `build/done.json`, `shader_cache/spirv`, `fonts/`, `assets/`,
   `upscalers/`, los `astcdec-*.dll`, y el dump.
3. En la Deck, en modo escritorio, abre una terminal (Konsole) en la carpeta del juego y ejecuta:

```bash
./pokopia-deck.sh setup /run/media/deck/<tarjeta>/pokopia/dump
```

   Como todo está hecho, solo reescribe las rutas de `pkp.ini` para que apunten al dump en la Deck (`Z:/run/media/...`) y termina enseguida.
   La primera vez Proton tarda un minuto en crear su prefijo.
4. Sigue en "Añadir el juego a Steam".

## Opción B: configurar en la propia Deck

Tarda horas (estimación: compilar `pkp.exe` 20-40 min, shaders 4-8 h). Se puede pausar y continuar.

1. Modo escritorio, Konsole, en la carpeta del juego:

```bash
./pokopia-deck.sh setup /run/media/deck/<tarjeta>/pokopia/dump
```

2. El progreso sale en la terminal y también en `setup_status.txt` (una línea, la última) y `setup.log` (todo) dentro de la carpeta del juego.
3. Para pausar: en otra terminal, `./pokopia-deck.sh pause` (o crea un fichero vacío llamado `setup.cancel` en la carpeta del juego). Vuelve a
   lanzar `setup` para continuar donde se quedó. Cerrar la terminal también interrumpe; el siguiente `setup` continúa igual, solo se pierde
   el lote de shaders en curso.
4. `./pokopia-deck.sh status` dice qué está hecho.

## Antes del primer arranque

```bash
./pokopia-deck.sh preset deck
```

Escribe `window_mode = borderless` en `controls.ini` (gamescope no da pantalla completa exclusiva). El resto de ajustes se cambian dentro del
juego con F1 (en la Deck, asigna F1 a un botón en la configuración de Steam Input, o usa el teclado virtual).

## Añadir el juego a Steam

1. Modo escritorio, Steam, Juegos, "Añadir un juego que no es de Steam", elige `pkp.exe` de la carpeta del juego.
2. Propiedades del acceso directo:
   - Opciones de lanzamiento: `--log pkp.log`
   - Compatibilidad: forzar Proton Experimental (o GE-Proton).
3. Vuelve al modo juego y arráncalo. `pkp.exe` lee todas las rutas de `pkp.ini`, así que no necesita más argumentos.

Alternativa desde la terminal en modo escritorio: `./pokopia-deck.sh play`.

## Qué mirar si algo falla

Todo queda en la carpeta del juego:

- `pkp.log`: el log del juego. Las líneas `GPU 0: ...` dicen si la GPU se acepta (`NOT USABLE` si no). Errores de `heap_create` o
  `MapViewOfFileEx` significan que Wine no deja reservar el espacio de memoria del juego. `xaudio2:` para el audio.
- `pkp_crash.txt`, `pkp_fatal.txt`, `pkp_freeze.txt`: informes de cierre, cuelgue o bloqueo.
- `setup.log`: la configuración.
- Si Proton no arranca el exe desde el script, prueba `PROTON=<ruta a proton> ./pokopia-deck.sh ...` con la ruta al script `proton` que uses.

Al reportar, adjunta `pkp.log` y di qué Proton usaste y si fue modo juego o escritorio.

## Compilar `pokopia-setup.exe` tú mismo

En Windows, con Rust instalado:

```bash
cd launcher/cli
cargo build --release
```

En Linux (SteamOS o cualquier distribución), compilación cruzada con mingw:

```bash
rustup target add x86_64-pc-windows-gnu
cd launcher/cli
cargo build --release --target x86_64-pc-windows-gnu
```

Hace falta `x86_64-w64-mingw32-gcc` en el PATH (paquete `gcc-mingw-w64-x86-64` en Debian/Ubuntu, `mingw-w64-gcc` en Arch). El resultado es
`target/x86_64-pc-windows-gnu/release/pokopia-setup.exe`.

## Limitaciones conocidas de esta fase

- Sin interfaz gráfica para la configuración; llega en la fase 2 del plan (lanzador nativo sin webview).
- Los tiempos de la opción B son estimaciones; la fase 4 del plan (herramientas nativas en SteamOS) los acortaría.
- DLSS no aplica en la Deck. FSR 3 sí (F1, Graphics).
