# Savage Wind

Savage Wind (repositorio *Savage Wind Recomp*) es una recompilación estática de *Breath of the Wild* (Switch)
para Windows y Linux. Parte de LASRecomp beta 3 (MIT), la recompilación estática de *Link's Awakening*.

Notas de desarrollo. El README explica qué hace falta y las reglas de copyright para quien llega al repositorio.

## Reglas legales del proyecto

- El repositorio y cualquier build no contienen archivos del juego, código del juego (tampoco el C
  generado), assets, fuentes del sistema ni keys.
- Savage Wind no desencripta nada. La entrada son archivos ya extraídos por el usuario desde su propia
  consola y su propia copia del juego, en `extracted/` (ignorado por git).
- La recompilación corre en la máquina del usuario (`make gen` o la pantalla de setup), igual que en
  LASRecomp beta 3.
- No se usan marcas de Nintendo en el nombre, los íconos ni los binarios del proyecto.
- `.gitignore` bloquea `dump/`, `keys/`, `*.keys`, `*.xci`, `*.nsp`, `*.nca` y similares.

## Publicación

GitHub: `Revivedx/SavageWindRecomp` (remote `origin`, público desde el 2026-10-16). No se sube el historial
local: la importación de LASRecomp trae capturas de *Link's Awakening* y su randomizer. El `main` remoto es la
rama local `public`, un commit por publicación con el árbol de `main` y el email noreply de GitHub:

```
export GIT_AUTHOR_NAME=Revivedx GIT_AUTHOR_EMAIL=82332536+Revivedx@users.noreply.github.com
export GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
git branch -f public $(git commit-tree "main^{tree}" -p public -m "<qué cambia>")
git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin public:main
```

Antes de cada publicación: `git ls-tree -r --name-only main` no debe tener archivos del juego, código generado,
keys ni rutas personales.

Releases: el zip de `make dist` (ver `docs/BUILD-windows.md`) se sube con `gh release create v<versión>
--prerelease` sobre el commit de `public`. Alpha 1 (`0.1.0-alpha.1`, 2026-10-17) se probó como jugador: zip
extraído en una carpeta nueva, PATH sin MSYS2, preparación desde la pantalla de inicio (copia, `romfs.bin`,
generación y compilación con swgen + zig: 2308 s con 12 hilos) y PLAY hasta tener a Link en el Santuario, a 30
fps. La carpeta queda en ~30,5 GB (romfs 15 GB, `romfs.bin` 15,1 GB, `swgame.dll` 209 MB), ~32 GB durante la
compilación. Para la prueba sirve `SAVAGEWIND_LAUNCHER_AUTOPLAY=1` con `SAVAGEWIND_HOST_INPUT` (la tecla E es A)
y `SAVAGEWIND_SCREENSHOT_EVERY` (capturas en `build/` de la carpeta de trabajo).

## Estructura de entrada esperada

```
savage_wind/extracted/
  exefs/   main, main.npdm, rtld, sdk (y subsdk* si el juego los trae)
  romfs/   contenido completo de la romfs (base + actualización ya fusionadas)
```

La versión objetivo es la 1.8.2 (versión de título v1048576), la última. El build id del módulo `main`
identifica la versión exacta; `tools/sw_probe.py` lo imprime.

## Cambios respecto de LASRecomp

Hechos:

- `tools/patches.py`: vacío. Los parches por instrucción de LASRecomp apuntaban a direcciones de
  Link's Awakening y habrían roto (o corrompido) la generación de BOTW.
- `tools/gen.py`: ya no carga `tools/rando/patch_variants.json` (sitios del randomizer de LA). El
  descubrimiento de funciones quedó en `discover()` para poder reusarlo desde otras herramientas.
- `tools/sw_probe.py`: nuevo. Mide el exefs sin escribir C (ver abajo).
- `.gitattributes`: finales de línea LF en checkouts de Windows.
- Runtime sin lo exclusivo de Link's Awakening (verificado con `make runtime-objs` en MSYS2 UCRT64:
  sin errores ni warnings, y el `.exe` enlaza):
  - Borrados: `runtime/rando.[ch]`; `runtime/ui/` `rando_tab`, `spoiler_view`, `tracker_overlay`,
    `inventory_overlay`, `pets_ui`, `chaos_overlay`, `save_editor`, `warp_tab`, `difficulty`, `cheats`,
    `shader_tour` (recorría los puntos de warp de LA) y sus datos `warp_data.h` / `item_names.h`;
    `runtime/hle/` `chamber`, `digs`, `doorjump`, `escape`, `flags_ext`, `hookshot`, `keysanity`,
    `miniboss`, `pets`, `pot_glow`, `qol`, `rescue`, `shopsanity`, `souls`, `warp`, `widescreen`;
    `runtime/nvn/texpatch.[ch]` (retoques de texturas de la UI de LA: iconos de botones Xbox/PlayStation,
    icono de Hero Mode, color de túnica y corazones, botella dorada).
  - Borrados fuera del runtime: `third_party/LAS-Randomizer`, `tools/rando/`, `tools/warp_table.py`,
    los objetivos `rando-helper` y `smoketest` del Makefile, `tools/linux/smoketest.sh`,
    `tools/doorjump_test.sh` y `tools/perf_sweep.py` (dependían del warp y del randomizer), keystone en
    `tools/build_deps_macos.sh`, el venv del randomizer en `tools/setup_windows.ps1`, y las capturas de
    LA de `docs/` (`ultrawide.jpg`, `start_screen.jpg`).
  - `make gen-helper` usa su propio venv (`build/gen-venv`: capstone, unicorn, lz4, PyInstaller);
    `tools/linux/rando_helper.sh` pasó a `tools/linux/gen_helper.sh` y solo congela el generador.
  - `tools/gen.py`: fuera el camino muerto de `rando_word()` (`RANDO_CODE` siempre estaba vacío).
  - Archivos genéricos revisados: `main.c` (sin seeds; `SAVAGEWIND_REBOOT_AT=<frame>:<game|title>`),
    `services/fs.c` (el overlay de la RomFS queda como herramienta de desarrollo:
    `SAVAGEWIND_ROMFS_OVERLAY=<carpeta>` reemplaza o agrega archivos; las partidas van siempre a `save/`),
    `hle/input.c` (sin ticks de LA; las variables de prueba que vivían en archivos de LA pasaron acá),
    backends Vulkan y Metal (sin pot/pets/widescreen/texpatch; la imagen queda siempre en 16:9 con
    barras), `settings.[ch]` (solo ajustes genéricos: video, audio, `dpad_move`, aviso del menú),
    `ui/menu.mm` (pestañas Controls, Video, Audio y Quit), `ui/launcher.mm` (sin selector de seeds,
    sin pregunta de aspecto, sin consulta de actualizaciones a GitHub de LASRecomp, sin fondo del video
    de título de LA; acepta cualquier `main` que sea NSO y muestra su build id). `paths.c`,
    `gamebuild.c`, `romfs_build.c`, `fnprof.c`, `hle/framepace.c` y `nvn/cmdbuf.c` no tenían ganchos
    de LA aparte de los nombres.
- Nombres: `savagewind[.exe]`, `savagewind.dll.a`, `swgame.{dll,so,dylib}` (entrada `swgame_tables`),
  generador congelado `swgen` (`tools/swgen.spec`), `savagewind.ini`, `savagewind.log`,
  `local.savagewind` (macOS), `~/.local/share/savagewind` (Linux), variables de entorno
  `SAVAGEWIND_*` (antes `LARECOMP_*`, incluida `SAVAGEWIND_DATA_ROOT`), "Savage Wind" en la UI.
- Módulos en memoria (era el pendiente "ajustar `MAIN_BASE`/`SDK_BASE` y cargar `subsdk*`"): ya no hay
  direcciones fijas por módulo; las calcula `tools/layout.py` y se cargan todos los `subsdk*`. Ver abajo.
- Arranque con el `nn::init::Start` de 1.8.2 (`_ZN2nn4init5StartEmmPFvvES2_S2_`, tres callbacks; el de
  LA tenía dos). Desensamblado, hace: `_init_libc0`, `nnosInitialize`, **x2**, `_init_libc1`,
  `nninitInitializeSdkModule`, `nninitStartup`, `_init_libc2`, **x3**, `nnMain`, **x4**,
  `nninitFinalizeSdkModule`, `nnosQuickExit`. `runtime/main.c` busca primero esa firma (si no, la de dos)
  y pasa x2 = inicializar `sdk`, x3 = inicializar `subsdk9`..`0` y `main`, x4 = finalizarlos en orden de
  carga (`DT_FINI`, o `fini_array` al revés); los destructores del `sdk` no corren (la salida es rápida).
  Para llegar a x2 hicieron falta además `DT_RELR` (abajo) y `svcGetInfo` 28 (`AliasRegionExtraSize`, 0).
- Jump tables en `tools/gen.py` (era el pendiente de `printf_core`, ver abajo).
  - Antes, el descubrimiento de funciones tomaba como función nueva el destino de un `adr` hecho dentro de la
    misma función. Ese destino es la base de una jump table: 611 en el `sdk`, 953 en `main` y 334 en `subsdk0`.
    Así partía la función, y los cortes se encadenaban: `printf_core` quedaba en 38 pedazos. El `br` caía en
    un pedazo sin sus casos, y el `switch` de respaldo solo cubre el pedazo: "no function at 0x700292070c".
  - Ahora el rango de cada función sale de su FDE de `.eh_frame` (`analyze.eh_frame_extents`, el
    `pc_range`). Un `adr`/`adrp`+`add` a una dirección de la misma función es una etiqueta, no un inicio.
  - El resolver busca la guarda, la base y los casos en toda esa función, no solo en el pedazo. Emula la
    ventana en línea recta: los `cbz`/`tbz` que el compilador intercala siguen de largo, y el `cmp` puede
    tener `cbz`/`tbz` entre él y la guarda. Una lectura por un registro sin valor lee ceros. La guarda puede
    estar hasta 32 instrucciones antes del `br`. Cubre tablas `ldrb`, `ldrh` y `ldrsw`.
  - Si una función queda partida igual (una llamada a su mitad), los casos que caen en otro pedazo pasan a
    ser inicios, y el `br` llega a ellos con una llamada de cola.
  - Resultado: ninguna función con FDE del `sdk` ni de `main` queda partida. Antes eran 443 y 788
    funciones, en 6102 y 8361 pedazos de más. En `subsdk0` queda una, partida por un `adrp`+`add` que
    apunta a la mitad de otra función. El descubrimiento termina en 2 rondas (antes 17 en el `sdk` y 7 en
    `main`). Hay 40461 funciones en el `sdk` (antes 46563) y 122961 en `main` (antes 131322).
  - Las estadísticas de `gen.py` y `sw_probe.py` cuentan ahora los `br` de las funciones finales. Antes
    sumaban los de cada ronda. Sitios `br`, antes → ahora:

    | módulo | jump tables resueltas | `br_all_labels` | `br_dispatch` |
    |---|---|---|---|
    | `sdk` | 0 → 555 | 611 → 56 | 9695 → 9695 |
    | `main` | 266 → 1288 | 1063 → 42 | 6343 → 6342 |
    | `subsdk0` | 0 → 333 | 333 → 1 | 4022 → 4021 |

  - `br_dispatch` es un salto por un puntero cargado (vtables, stubs), no una tabla. El que cambia en `main`
    es una tabla `ldrsw` que el corte dejaba en un pedazo sin su `add`.
  - Los 56 + 42 que siguen sin resolver tienen la guarda en un bloque anterior, el índice recargado de otra
    ranura de la pila, o el índice acotado con `and`/`ubfx` sin `cmp`. Usan el respaldo de siempre: cada
    instrucción de la función es una etiqueta y el `br` salta por un `switch`. Como la función ya no está
    partida, igual llegan.
  - Costo: el C con todas las etiquetas crece en `main` de 312k a 485k instrucciones y en el `sdk` de 37k a
    171k. La función más grande sigue siendo la misma (11883 instrucciones, en `main`).
  - `tools/test_layout.py` agrega un módulo sintético con la forma de `printf_core`, y una función partida
    por una llamada a su mitad.

Pendiente:

1. Restos de LA conocidos, sin efecto en la compilación:
   - `ui/launcher.mm` no comprueba la versión: falta el build id del `main` 1.8.2 (`tools/sw_probe.py`).
   - `hle/movie.c` solo deja saltar `movie/opening.webm` (nombre de LA).
   - `hle/framepace.c` y `hle/heaps.c` explican sus valores con casos de LA (siguen siendo genéricos).
   - `CONTRIBUTING.md` es de LASRecomp. (El README y el ícono ya son propios: `docs/icon/icon.svg`, una montaña
     helada con una espada clavada en la cima, dibujo original; `icon_1024.png`, `icon.ico` e `icon.icns` salen de
     él con `rsvg-convert`.)
   - `tools/explore_far.sh`, `explore_input.sh`, `house_input.txt`, `dbgfs.sh`, `fresh_save.sh`: entradas
     de prueba pensadas para la pantalla de título de LA.
   - Pantalla ancha (aspecto desbloqueado) e iconos de botones por control: se fueron con
     `widescreen.c` y `texpatch.c`; para BOTW harían falta ganchos propios.
   - `runtime/nvn/metal.m` y `renderer.m` (solo macOS) se editaron sin compilarlos.
2. Arranque de 1.8.2: con el `sdk` generado entero, x2 corre los constructores del `sdk`.
   - Pasa por `printf_core`. Arreglado en `gen.py`: ver "Jump tables" arriba. Medido: entra 4 veces (dos
     `vfprintf` con `"%s"` y `"%02d - %s"`), y su tabla `ldrb` salta a 0x700292070c y a 0x700292063c.
   - Resuelto (2026-10-14), en orden de aparición:
     - TIPC: el `sdk` de 1.8.2 le habla a `sm:` por TIPC (tipo de mensaje 16 + comando; 15 cierra la sesión).
       `runtime/ipc.c` lo atiende con el mismo `sm_handler`: los argumentos son las palabras crudas, sin
       `SFCI`, y la respuesta lleva el resultado como primera palabra, sin `SFCO` ni alineación.
     - `romfs.bin`: el runtime lee la RomFS desde esa imagen, que arma la pantalla de setup. Para
       desarrollo sin launcher: `build/savagewind.exe extracted --build-romfs` (4,5 min, 15,1 GB).
     - `IApplicationFunctions` 210 (`GetHealthWarningDisappearedSystemEvent`): el juego espera ese evento
       antes del primer frame; el stub no devolvía handle y el juego sondeaba el handle 0 para siempre.
       Ahora devuelve un evento ya señalado.
     - Versión de NVN: `main` 0x7100d802e0 solo crea su sistema gráfico si el dispositivo informa API 55 con
       minor ≥ 15; con el 53.207 de LA seguía sin él y después leía un objeto nulo. Además pide las
       propiedades 82, 85 y 92 (tamaños por defecto de memoria de comandos, compute y control de la cola).
     - `NVNsamplerBuilder` mide 64 bytes en NVN 55. El HLE guardaba 80 (device + `NvnSamplerDesc`) y pisaba
       el frame pointer guardado del llamador (`main` 0x71000156f0 lo arma en la pila). El estado del
       builder ahora está empaquetado en 48 bytes.
   - Resuelto (2026-10-15 y 16), en orden de aparición:
     - Lista rota del heap de efectos (`main` 0x7100d8df90 giraba para siempre): la escribía
       `hle_nvnTextureViewSetDefaults`. `NVNtextureView` mide 16 bytes en NVN 55 y el HLE guardaba 0x28; el
       juego pone las vistas al final de bloques de un `sead::ExpHeap` y pisaba la cabecera del bloque
       siguiente. `NvnTextureView` ahora está empaquetado en 16 bytes. Lo encontró la trampa de escrituras
       por página (`SAVAGEWIND_WATCH*`, abajo).
     - Tamaños reales de NVN 55, leídos de los `SetDefaults`/`Initialize` del driver del `sdk` (sus funciones
       se ubican con `SAVAGEWIND_NVN_REAL=1`): `TextureView` 16, `SamplerBuilder` ≥ 48, `QueueBuilder` 60,
       `WindowBuilder` 48, `BufferBuilder` 40, `DeviceBuilder` 24, `EventBuilder` 16, `MemoryPoolBuilder` 64,
       `TextureBuilder` ≥ 104; objetos: `Sampler` ≥ 64, pools 32, `Event` 40, `Buffer` 48,
       `CommandBuffer` 160, `MemoryPool` 256, `Program` 128, `Sync` ≥ 56.
     - `nvnDeviceGetInteger`: la tabla de LA se reemplazó por la del driver 55.15 (`sdk` 0x71004c2c00 →
       0x71004c6310 → tabla 0x71004b6070); los valores que dependen del dispositivo son los de un Tegra X1.
     - Audio: con revisión ≥ 9 el SDK lee la versión 2 del estado de efectos (0x90 bytes cada uno) y la
       sección de rendimiento de 0x10; con otro tamaño abortaba la actualización (0x5299).
     - `nfp:user` (amiibo): `IUserManager` → `IUser` sin lectores; antes el juego leía un objeto nulo.
     - `svcGetInfo` 18 (`ProgramId`) sale de `main.npdm` y 20 (`UserExceptionContextAddress`) devuelve una
       zona de TLS: `SetDyingMessageRegion` la necesita no nula. El id base de `aoc:u` sale del program id.
     - Memoria de comandos: el juego graba cada display list una vez para medirla ("calc resource") y pide
       exactamente esa memoria al heap del modelo, que la `ResourceSizeTable` dimensiona para NVN real. Con
       los records de Savage Wind (≥ 80 bytes por comando) los heaps se quedaban cortos, la asignación fallaba
       y la lista llegaba con pool nulo. Ahora el juego ve un costo estimado por comando (≤ lo que escribe NVN)
       y los records van a memoria del host, en "sombras" por rango de memoria guest (`runtime/nvn/cmdbuf.c`).
     - Hilos: BOTW llama a NVN desde varios hilos. El renderer (submit, flush, present, texturas finalizadas)
       queda detrás de un lock recursivo (`gpu_lock`).
     - Texturas: los comandos guardan el `NVNtexture` del guest y lo resuelven al ejecutarse; el juego
       finaliza y vuelve a inicializar render targets en el mismo objeto, copia objetos `NVNtexture` y
       finaliza una copia mientras usa otra. `nvnTextureFinalize` solo suelta la imagen Vulkan; el objeto del
       host vive hasta un reinicio.
     - Buffers de onda fuera de la memoria guest se saltean en vez de leerse; pools de descriptores: hasta 64
       por envío.
   - Resuelto (2026-10-16, segunda tanda):
     - Shaders: `TLD4`/`TLD4S` (gather), `TMML` (LOD), formas bindless con el handle cargado de `c[2]` justo
       antes, y `LDS`/`STS` (memoria compartida de compute, tamaño en la sección de control +0x754). Todos los
       shaders del arranque traducen.
     - `nvnMemoryPoolMapVirtual`: el juego mapea su pool virtual de texturas (0x7300000) entero sobre uno físico;
       el pool virtual toma esa memoria.
     - Render targets re-creados cada frame: BOTW finaliza e inicializa otra vez sus targets en la misma memoria
       y muestrea los objetos viejos (entradas del texture pool, copias de `NVNtexture`). La imagen de una textura
       finalizada se estaciona y la toma la próxima textura con la misma memoria y forma; un objeto finalizado se
       resuelve a la textura hecha después en su memoria. Era la imagen "en bloques".
     - Cuelgue intermitente (la mitad de las corridas, entre los frames 280 y 1500): `svcWaitProcessWideKeyAtomic`
       soltaba el mutex antes de marcar el condvar, y quien señalaba se salteaba `svcSignalProcessWideKey`.
     - Entrada: BOTW lee los controles con `nn::hid::GetNpadStates` (plural), que el HLE no atendía.
     - Imágenes de texturas finalizadas que se seguían muestreando no se liberaban (el menú agotaba la memoria de
       video en 2 minutos).
     - Escala de render targets (la "resolución interna" de LA): apagada salvo `SAVAGEWIND_SCALE_TARGETS=1`; con
       ella el pipeline diferido de BOTW mostraba solo lo emisivo.
   - Resuelto (2026-10-16, tercera tanda): la iluminación diferida del santuario. En orden de impacto:
     - Listas pregrabadas que se perdían. El juego da a la lista de material de cada composición de luz una
       ventana de 0x200 bytes de memoria de comandos, graba los ~0x18 bytes que midió antes y empaqueta la
       siguiente justo detrás. El HLE daba por muerta toda lista que se solapara con la ventana nueva aunque nada
       se escribiera encima, y el frame la seguía llamando: ejecutaba otros records. Así se perdía el uniform
       buffer con el ID de material (`c4` del VS, 432 bytes, el ID en +0x1a8), que las composiciones comparan
       con test EQUAL contra la D16 que escribe el G-buffer: no se componía ninguna luz. Ahora una lista muere
       cuando la grabación escribe de verdad sobre su memoria (`cmdbuf.c`, `shadow_claim`).
     - `nvnCommandBufferBindTexturesDeferred` (el stub no ataba nada): el juego ata así las texturas de la
       composición (normales, albedo, profundidad lineal, probe, buffers de luz). Lee los handles al ejecutar.
     - Las texturas de los comandos se resuelven al grabar (`nvn_texture_host`), como en el hardware. La cadena
       de exposición (192x192 → … → 3x3) reinicializa el mismo `NVNtexture` en cada paso dentro de un command
       buffer: resueltas al ejecutar, todos los pasos dibujaban en el último.
     - Una imagen Vulkan por memoria y forma (`textures.c`): los objetos `NVNtexture` sobre la misma memoria con
       la misma forma comparten imagen (entradas del pool con objetos finalizados, copias de objetos, targets
       recreados). Reemplaza el estacionamiento de imágenes y `nvn_texture_current`.
     - Cubemaps muestreados como cube array (probes de 64x64 R11G11B10): salían negros.
   - Resuelto (2026-10-16, audio):
     - El juego mezcla con el AudioRenderer de su propio `sdk` (servidor y DSP recompilados) en vez del HLE de
       `audren:u`, que con el SDK de 1.8.2 recibía voces con formato 0 y buffers sin sentido y quedaba mudo
       (sección "Audio").
     - Kernel: un `svcWaitProcessWideKeyAtomic` que vencía volvía con el mutex tomado, y el SDK lo vuelve a tomar
       él mismo (`InternalConditionVariableImplByHorizon::TimedWait`, `sdk` 0x71001492bc, como en la consola): el
       hilo esperaba su propio mutex y colgaba a los demás (el hilo de audio del juego, a los 30 s). Ahora vuelve
       sin el mutex. Tampoco cuenta como vencido un timeout que llega cuando otro hilo ya lo despertó o le dio el
       mutex.
     - Kernel: `svcArbitrateUnlock` y `svcSignalProcessWideKey` ceden el núcleo después de soltar, no antes: el
       hilo que cedía primero dejaba esperando a los demás hasta recuperar su núcleo.
   - Resuelto (2026-10-16, santuario):
     - El pasillo marrón que lleva al pedestal se veía estirado a lo largo en casi todas las corridas: su textura
       (1024x2048, UV que se repite) se muestreaba con un sampler de clamp. El juego copia objetos `NVNsampler`
       por valor y registra la copia en el pool después de finalizar el original; cada objeto apuntaba a una
       descripción propia que `nvnSamplerFinalize` liberaba, y la copia leía la memoria ya reusada por otro sampler.
       Ahora las descripciones se comparten por configuración y no se liberan (`objects.c`).
     - El shader de partículas que fallaba con "bindless tmml (handle not from c[2])" traduce (una burbuja pequeña
       del santuario); no tenía que ver con lo anterior.
   - Ahora: el menú principal se ve bien. Con `Continue` (`SAVAGEWIND_INPUT="900:A:8,1400:A:8,1900:A:8"`) el
     Santuario de la Resurrección se ve iluminado: Link, el pedestal, la puerta, los pilares y el título del
     lugar. Lo conocido:
     - Niebla azul muy fuerte; en algunas corridas, un rectángulo arriba a la izquierda (~1/4,5 de la
       pantalla) con otro tono y una franja de bruma blanca. Vienen de los efectos a media resolución que se
       componen sobre el HDR (pases de niebla/volumen después de la luz).
     - 6-8 fps en el santuario (~3.400 draws por frame). Con `SAVAGEWIND_VK_PROF=1` el frame es ~20 ms de
       backend, ~25 de presentación y ~75 del lado del juego; no cambia con el arreglo de listas.
     - En algunos frames, una textura 3D de 32x32x32 (BC4) se muestrea como 2D y devuelve negro
       (`[tex] type mismatch`).
     - El resto de los `nvnCommandBufferBind*Deferred` (imágenes, uniform y storage buffers, texturas y
       samplers separados) sigue sin implementar: el juego los busca pero no los llamó en estas corridas.
       `renderer.m` (Metal) no ejecuta `CMD_BIND_TEXTURES_DEFERRED`.
     - Sonido: suenan el jingle de `Continue` (`Sys_GameStart.bfstm`) y el ambiente del Santuario. En el título y
       el menú el juego no pide ninguna voz (ni un `AcquireVoiceSlot` en 50 s, y no lee nada de
       `Sound/Resource`), así que ahí no suena nada.
   - Teclado y ratón (2026-10-16): defaults pensados para BOTW (tabla en el README), cualquier tecla o botón del
     ratón remapeable (`BIND_MOUSE` + botón en `g_bind_key`), acción "Walk (hold)" (C: el stick izquierdo a 40%) y
     el ratón como cámara: con la ventana enfocada, sin menú y sin script, el cursor queda retenido (modo relativo de
     SDL) y su velocidad se vuelve el stick derecho (≈1200 px/s a fondo con sensibilidad 1, más un 12% para pasar la
     zona muerta del juego; `SAVAGEWIND_MOUSE_SCALE`, `SAVAGEWIND_MOUSE_DEADZONE`). Esc y F1 siguen abriendo el
     menú, que suelta el cursor. Ajustes: `mouse_camera`, `mouse_sensitivity`, `mouse_invert_y`.
   - Ritmo de frames (2026-10-16): `nvnQueuePresentTexture` espera el turno de cada frame según el intervalo de la
     ventana (vsyncs por frame) y nunca presenta más rápido que el tope (`SAVAGEWIND_FPS_CAP`, 60 por defecto; 0 sin
     tope). BOTW pide intervalo 2 (30 FPS, como en la consola; 3 en algunas cargas): el menú, que iba a 130 FPS con
     sus animaciones aceleradas, va a 30. Subir de 30 necesita parchear el paso de tiempo del juego.
   - Overlay de rendimiento (F10: off, FPS, full; ajuste `perf_overlay`): FPS y ms del juego con gráfico, CPU (total
     y del juego), RAM, GPU (uso, temperatura, potencia, reloj, VRAM) por NVML, que trae el driver de NVIDIA
     (cargado en tiempo de ejecución), y en Linux amdgpu por sysfs. Temperatura de CPU: hwmon en Linux; en Windows
     solo la zona térmica ACPI (este equipo no la tiene): la del encapsulado necesita un driver de kernel, que Savage
     Wind no instala. `runtime/perfmon.c`.
   - Idiomas (2026-10-16): pestaña Language del menú, ajustes `text_language` (región-idioma de BOTW: USen, EUes,
     JPja, ...; `auto`: el idioma de la interfaz del sistema, inglés si el juego no lo tiene) y `voice_language`
     (carpeta de `Voice/`; `text`: las del idioma del texto). El texto es lo que responden `am`
     `GetDesiredLanguage` y `set` `GetLanguageCode` (es-419 carga `Pack/Bootup_USes.pack` y `TitleBG_USes`). Las
     voces se redirigen al indexar el RomFS (`voice_redirect` en `runtime/services/fs.c`): cada
     `Voice/<idioma>/<ruta>` salvo `Common` toma los datos de la misma ruta en la carpeta elegida (los `.bars` y los
     diálogos `Stream_*/…bfstm`, 6441 archivos con inglés), parcheando solo la tabla de archivos en la copia que ya
     usa el overlay de mods. Ambos se leen al arrancar el juego: en juego, "Restart now" (reinicio en el sitio)
     los aplica.
   - Menús traducidos (2026-10-16): la pantalla de inicio y el menú del juego siguen al idioma de texto (Auto: el
     del sistema). `tr("texto en inglés")` (`runtime/ui/i18n.mm`) busca el texto en la tabla del idioma,
     `runtime/ui/i18n/<idioma>.inc` (`T("English", "...")`; `""` deja el inglés). Los widgets de
     `option_rows.mm` y los helpers de `menu.mm` / `launcher.mm` traducen etiquetas, valores, descripciones y
     títulos ellos mismos; los IDs de ImGui siguen en inglés. Mayúsculas en UTF-8 (`tr_upper`: latín, griego,
     cirílico). Ruso, japonés, coreano y chino suman una fuente del sistema con solo los glifos que usan sus
     textos (`ui_merge_fallback_font`), y cada contexto rehace sus fuentes al cambiar el idioma.
     `tools/i18n_check.py` lista los textos que le faltan a cada tabla (`--missing <idioma>` los imprime como
     líneas `T(...)`); `SAVAGEWIND_MENU_LANG=<código>` (p. ej. `ja`) fuerza el idioma de los menús para probar.
     Las once tablas están completas (es, fr, de, it, nl, ru, pt, ja, ko, zh-Hans, zh-Hant). ImGui 1.91.9 solo
     corta líneas en espacios: `third_party/imgui/imgui_draw.cpp` (`ImFont::CalcWordWrapPositionA`) lleva un
     parche local que también corta entre caracteres CJK, respetando la puntuación.
   - setjmp / longjmp (2026-10-16): libjpeg y libpng de `nn::image` salen de sus errores con `longjmp`, que vuelve
     a la mitad de la función que llamó a `setjmp`; el código recompilado no puede retomar ahí ("no function at
     70023f1b14", al codificar la miniatura del guardado). `tools/gen.py` envuelve cada `bl` a `setjmp` (el
     símbolo o su stub PLT) en un `setjmp` del host dentro de la función C que llama (`setjmp_call`), con un
     `jmp_buf` del host por cada `jmp_buf` del juego y por hilo (`rt_setjmp_slot`). El `setjmp` de musl sigue
     corriendo y llena el del juego; `longjmp` es HLE (`hle_longjmp`, `runtime/hle/setjmp.c`): restaura x19-x30,
     sp y d8-d15 de ese `jmp_buf` y salta al del host. Solo el `sdk` los usa (10 llamadas).
   - Compilación de shaders síncrona por defecto (2026-10-16): un draw cuyo programa o pipeline todavía compilaba
     en otro hilo se descartaba. Lo que el juego dibuja una sola vez se perdía: la miniatura del guardado (dos
     blits, 1920x1080 a 854x480 a 256x144, leídos enseguida con `CopyTextureToBuffer`) salía con lo que hubiera en
     esa memoria de la GPU. Ahora el draw espera la compilación (un tirón la primera vez que aparece un efecto; el
     caché de SPIR-V y de pipelines lo evita en las siguientes sesiones). Ajuste "Asynchronous shaders"
     (`async_shaders`) para volver a no esperar; `SAVAGEWIND_SYNC_SHADERS` / `SAVAGEWIND_ASYNC_SHADERS` mandan
     sobre el ajuste.
   - Herramientas de diagnóstico:
     - `SAVAGEWIND_THREAD_DUMP_AT=<s>,<s>`: estado, núcleo y cadena de llamadas de cada hilo; para los que
       esperan un mutex, también su palabra (el handle del dueño).
     - Audio: `SAVAGEWIND_AUDIO_LOG=1` (cada 10 s: tiempo de render, espera del mutex del renderer, cortes del
       dispositivo, pico), `SAVAGEWIND_AUDIO_WAV=<archivo>` (graba la salida), `SAVAGEWIND_AUDIO_DFC=0` (vuelve
       al HLE de `audren:u`).
     - `SAVAGEWIND_WATCH=<dir>:<largo>` (con `_DEREF`, `_TRAP`, `_STEP`, `_FOLLOW`, `_WRITES`): trampa de
       escritura por página con paso a paso (Windows), y quién escribió (`módulo+offset`).
     - `SAVAGEWIND_FNPROF=0:999999` con `SAVAGEWIND_FNPROF_ARGS=<dir>,...`: x0-x5 y el llamador en cada
       entrada de esas funciones; `SAVAGEWIND_FNPROF_DEREF=<reg>:<n>` vuelca lo apuntado (y muestra textos).
     - `SAVAGEWIND_NVN_LOG=1`: funciones NVN que el juego busca y enteros del dispositivo que lee.
     - `SAVAGEWIND_CORES=0` / `SAVAGEWIND_ONE_CORE=1`: sin emulación de núcleos / todo en el núcleo 0.
     - `SAVAGEWIND_SHOT_AT=<frame>,...` (capturas en `build/frames/`) y `SAVAGEWIND_DEBUG_FRAME=<frame>`
       (cada pase en `build/pass_NNN.png`, `_c<i>` para los demás targets; un frame del juego guarda cientos de
       imágenes y tarda minutos). `SAVAGEWIND_HIDDEN=1` corre sin ventana visible.
     - `SAVAGEWIND_SHADER_DUMP=1`: el código Maxwell de los shaders que no traducen, en `build/shaders/`.
     - `SAVAGEWIND_DEBUG_PASS_FROM=<n>`, `SAVAGEWIND_DEBUG_PASS_TO=<n>` y `SAVAGEWIND_DEBUG_MAIN_ONLY=1` (con el
       frame de depuración, F9): guarda solo esos pases y solo el primer target de cada uno. Con
       `SAVAGEWIND_DEBUG_FRAMES=<n>`, n frames seguidos (`build/pass_<frame>_NNN.png`).
     - F12: guarda una imagen del juego de cada dos en `build/frames/rec_N.png` hasta el siguiente F12, para ver
       parpadeos de un frame (el juego va más lento mientras graba).
     - `SAVAGEWIND_TRACE_TEX=<an>x<al>[,...]` o `all`: cada textura (por su memoria) guarda sus últimas escrituras
       (draws con el shader, el uniform buffer del VS y las texturas que muestrean; copias; subidas desde la memoria
       del juego). Al copiarse una a un buffer (la CPU la lee: una captura) se loguea su historia y la de sus
       fuentes, y se guardan como `build/trace_<n>_<id>.png`. `all` baja el juego a ~20 FPS.
     - `SAVAGEWIND_LOG_JPEG=1` (o `dump`, que guarda la primera imagen en `build/jpeg_in.raw`): cada
       `JpegEncoder::Encode` con su tamaño, calidad, espacio y resultado, y de dónde vienen los píxeles. BOTW baja
       la calidad de 5 en 5 hasta que la miniatura entra en 10 KB. `SAVAGEWIND_LOG_LONGJMP=1`: cada `longjmp`.
     - En el frame de depuración: `[draw]` muestra la `VkImage` de cada target y textura, los factores de blend,
       el depth range y el polygon offset; `SAVAGEWIND_DEBUG_DATA=1` vuelca también los cbufs del VS (y sus
       constantes `c1`) de los draws de pantalla completa; `SAVAGEWIND_LOG_BINDS=1`, los binds de texturas y
       uniform buffers y las actualizaciones inline; `SAVAGEWIND_LOG_OPS=1`, cada record ejecutado, los `CALL`
       y los envíos.
     - `SAVAGEWIND_DBG_FS=<fs hex>:<pc hex>:<reg>`: ese registro tras esa instrucción como salida del fragment
       shader (`pc` ffffffff: 1.0 donde el shader corre), ahora también en Vulkan. `SAVAGEWIND_SAVE_MSL=1`
       guarda el GLSL de cada shader en `build/glsl/`.
     - Nombrar un fragment shader sin su dirección (cambia en cada corrida): en `SAVAGEWIND_SKIP_FSHASH`,
       `SAVAGEWIND_DBG_FS` y `SAVAGEWIND_DEBUG_TEXDUMP` vale `w<pc>=<palabra>`, los shaders cuya instrucción en ese
       `pc` es esa palabra (los comentarios `// pc` del GLSL guardado), además del hash del log `[draw]`.
     - F8: dibuja / vuelve a saltar los shaders de `SAVAGEWIND_SKIP_FSHASH` en vivo (A/B en la misma vista). F10 es
       el overlay de rendimiento.
     - `SAVAGEWIND_DEBUG_TEXDUMP=<fs>` (con F9): el primer draw de ese shader guarda sus texturas canal por canal
       y capa por capa (`build/dtex_<hash>_<i>[_L<capa>]_<r|g|b|a>.png`, profundidad en `.pgm`), su layout de
       vértices con los primeros vértices en hexadecimal, y sus uniform buffers enteros. Con
       `SAVAGEWIND_DEBUG_TEXDUMP_ALL=1`, cada draw de ese shader guarda además sus texturas
       (`build/dtex_<hash>_d<draw>_<i>.png`). El log `[draw]` del frame muestra el sampler (filtros, wrap, LOD),
       el handle y el swizzle de cada textura, su forma (capas) y las vistas de los render targets.
     - `SAVAGEWIND_DEBUG_PICK=<x>,<y>[;<x>,<y>...]` (hasta 4 puntos en coordenadas de 1600x900, con F9): después
       de cada draw copia esos píxeles de su primer target; al terminar el frame, `[pick]` lista por punto los
       draws que los cambiaron (el último es el que se ve) con su shader y hash.
     - En Git Bash, `VAR=1 timeout 60 ./build/savagewind.exe` no le pasa la variable al programa: usar
       `timeout 60 env VAR=1 ./build/savagewind.exe`.
     - `tools/ghidra/gq.py data <dirección> [n]` (palabras de 64 bits y a qué apuntan). Con gdb de MSYS2
       (`gdb -p <pid>`) se ve la pila nativa con los nombres `f_<dirección>` de `swgame.dll`, y la memoria del
       juego se lee en las mismas direcciones.

Se conserva todo lo genérico: el lifter AArch64 → C, el kernel HLE, IPC y servicios, el HLE de NVN
con el backend Vulkan, el traductor de shaders Maxwell, ASTC, audio, la UI de ajustes y la
compilación local con `zig cc`.

## Módulos en memoria

Las direcciones de los módulos del exefs se deciden en un solo lugar, `tools/layout.py`, igual que el
loader de Switch: uno detrás de otro en orden de carga (`main`, `subsdk0`..`subsdk9`, `sdk`; `rtld` no
se carga, su trabajo lo hace el runtime), cada imagen (text, rodata, data + bss) redondeada a 4 KiB,
desde `MAIN_BASE` (0x70_0000_0000) y sin pasar de `MODULES_END` (0x70_8000_0000: 2 GiB para módulos).
Ambos límites están en `runtime/rt.h` y en `tools/layout.py`.

- `make gen` (`gen.py all`) escribe `generated/layout.txt` y `generated/layout.c` y genera cada módulo en
  `generated/<módulo>/`. `gen.py <módulo> <nso> <layout.txt> <salida>` genera uno solo con esas
  direcciones y se niega si el NSO no es el del layout (build id).
- `layout.c` se compila dentro de la biblioteca del juego (`runtime/gamelib.h`, `GAMELIB_ABI` 2): por
  cada módulo, nombre, dirección, tamaño, build id y tabla de funciones. `runtime/loader.c` carga cada
  módulo donde dice la biblioteca y se detiene si un NSO tiene otro build id o si hay un `subsdk*` que
  la biblioteca no tiene (hay que volver a generar).
- La pantalla de setup (`runtime/gamebuild.c`) corre `gen layout` y después un generador por módulo.
- `tools/sw_probe.py` imprime la misma disposición y si el runtime la puede cargar.
- `tools/nso.py` y `runtime/loader.c` leen `DT_RELR` (relocaciones relativas empaquetadas): en 1.8.2 todas
  las `R_RELATIVE` de los tres módulos están ahí (222402 en `main`, 3424 en `subsdk0`, 15007 en `sdk`).
  Sin esto los punteros de los datos quedaban sin reubicar. `gen.py` también los toma ahora como inicios
  de función posibles, pero casi todos ya los encontraba por otras vías (eh_frame, `bl`, `adrp`+`add`):
  en `main` agregan 2.
- `tools/nso.py` y `runtime/loader.c` cuentan los símbolos también con `DT_GNU_HASH`: el `sdk` y el
  `subsdk0` de 1.8.2 no traen `DT_HASH`, y sin esto se veían con 0 símbolos: los imports de `main` no
  se resolvían y `gen.py --hle` no podía encontrar funciones del `sdk` (hoy reemplaza `longjmp`, ver
  "setjmp / longjmp").
- `tools/test_layout.py` lo prueba con NSOs sintéticos sin código del juego (un `main` de 77 MB, dos
  `subsdk`, un `sdk` de tamaño impar, `subsdk0` y `sdk` solo con `DT_GNU_HASH` y `DT_RELR`):
  `tools/venv/Scripts/python tools/test_layout.py`.

Con el exefs de 1.8.2: `main` en 0x70_0000_0000 (29.2 MiB), `subsdk0` (multimedia) en 0x70_01d3_7000
(6.4 MiB), `sdk` en 0x70_023a_3000 (12.3 MiB). El loader carga los tres ahí, con 488, 10110 y 23039
símbolos; ningún import queda sin resolver salvo los `glslc*` débiles de `main`.

## Audio

El `sdk` de BOTW trae el AudioRenderer entero: el servidor (`nn::audio::server`: voces, mezclas, efectos,
generación de comandos) y el procesador de comandos del DSP (`nn::audio::dsp::CommandListProcessor`), código de
CPU que se recompila con el resto. Savage Wind lo usa en vez de reimplementarlo (`runtime/services/audio.c`):

- El cliente llega al servidor dentro del proceso (`CreateAudioRendererManagerByDfc`, `sdk` 0x7100224d00) cuando
  los parámetros piden el dispositivo de render CPU: `nn::audio::OpenAudioRenderer` (`sdk` 0x7100220a70) toma ese
  camino si `AudioRendererParameter` +0x2c es 1, y con +0x30 (modo de ejecución) en 1 lo abre en ejecución manual,
  sobre el work buffer del juego, sin transfer memory. Si no, abre `audren:u`.
- El HLE de `InitializeAudioRendererParameter` pide CPU + manual, así que el juego calcula su work buffer y abre el
  renderer con esos valores. El de `OpenAudioRenderer` guarda el handle y arranca dos hilos `nn::os` (el código
  del `sdk` busca su `ThreadType` en TLS +0x1f8): uno renderiza un frame de 5 ms
  (`AudioRenderSystem::ExecuteAudioRendererRendering`, en el objeto del renderer +0x70) cada vez que el stream de
  SDL baja de su objetivo, con el mutex del renderer (+0x2d0, el que toma `Update`) tomado; el otro señala el
  evento del juego, para que el render no espere el mutex del evento.
- Esos dos hilos hacen el trabajo del ADSP y corren fuera de los tokens de núcleo (`kernel_thread_off_cores`):
  el juego también usa el núcleo 3, porque el kernel informa la máscara 0xF.
- La mezcla final es un comando DeviceSink (id 0x14), que el procesador del `sdk` salta (en la consola lo toca el
  ADSP): un hook después de `CommandListProcessor::Process` lee sus buffers (int32 en la escala de int16, 6
  canales FL FR FC LFE BL BR), los baja a estéreo y los manda al stream SDL.
- Medido en el Santuario (Ryzen 5 5600X): unos 190 µs por frame (máximo 0,7 ms), espera del mutex casi siempre
  por debajo de 2 ms, 4 cortes del dispositivo en 150 s con 25 ms en cola, todos mientras carga la escena (el
  `Update` del juego retuvo el mutex hasta 30 ms). Con los hilos dentro de los tokens de
  núcleo y el evento señalado desde el mismo hilo eran 1-2 ms por frame, picos de 50-170 ms y decenas de cortes
  por segundo.
- La idea viene de SuperMarioWonderRecomp (MIT, BelmanteGu), que corre así el renderer del SDK 16 de Wonder; se
  usó su PROGRESS.md como referencia, sin copiar código.
- `SAVAGEWIND_AUDIO_DFC=0` vuelve al mezclador HLE de `audren:u` (heredado de LASRecomp), que sigue en el archivo.
  Sin probar: el reinicio en el lugar (vuelta a la pantalla de inicio) con el renderer abierto, y
  `CloseAudioRenderer`, que el juego no llamó en las pruebas.

## Sondeo del exefs

```
tools/venv/Scripts/python tools/sw_probe.py extracted/exefs --json build/probe.json
```

(En Linux: `tools/venv/bin/python`.) Imprime las direcciones que `tools/layout.py` da a los módulos y
si el runtime puede cargarlos ahí. Para cada módulo imprime:

- tamaños de segmentos y build id,
- funciones encontradas y jump tables,
- instrucciones que el lifter no traduce todavía, agrupadas por mnemónico con un ejemplo,
- tamaño estimado del C que generaría `make gen`,
- las funciones NVN que el juego nombra contra las que implementa `runtime/nvn` (faltantes y stubs).

El entorno Python se crea con:

```
python -m venv tools/venv
tools/venv/Scripts/python -m pip install capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
```

## Toolchain

Para compilar el runtime en Windows hace falta MSYS2 UCRT64 (clang, make, SDL3, FFmpeg, Vulkan,
shaderc), como describe `docs/BUILD-windows.md`. En Linux, los paquetes de `docs/linux.md`.
Comprobación rápida desde una shell UCRT64: `make runtime-objs` (compila el runtime sin enlazar).

## Primera generación y compilación completa (1.8.2)

Medido el 2026-10-14 con el exefs 1.8.2 (`main` build `de0b6ac4efc1dda7`, `subsdk0` `3308c1d216f78839`,
`sdk` `5cf080e800530a7f`) en un Ryzen 5 5600X (6 núcleos, 12 hilos) con 32 GB de RAM, Windows 11,
MSYS2 UCRT64 clang 23.1.3 y Python 3.14 en `tools/venv`. Usa el generador con el layout de módulos,
`DT_GNU_HASH` y `DT_RELR`. La biblioteca del juego (`build/swgame.dll`) compila y enlaza sin errores ni
warnings: nada del generador ni del Makefile la bloqueó. El juego no se corrió con ella (el arranque está
en "Pendiente").

Generación (`make gen`, es decir `gen.py all`: un módulo detrás de otro; `--simd c`, el default en x86-64):

| | `main` | `subsdk0` | `sdk` | total |
|---|---:|---:|---:|---:|
| funciones | 131.322 | 17.790 | 46.563 | 195.675 |
| archivos `.c` (con la tabla) | 384 | 98 | 125 | 607 + `layout.c` |
| C generado | 375,7 MB | 60,2 MB | 115,9 MB | 551,8 MB |
| tiempo | 42 s | 6 s | 13 s | 62 s |

La memoria pico de Python es la de `main`: 1,9 GB. `sw_probe.py` había estimado 449 MB para `main` +
`sdk`; el real es 491,5 MB, un 9 % más. La función más grande ocupa 1,68 MB de C. 114 archivos de `main`,
38 de `subsdk0` y 31 de `sdk` tienen una sola función (`--big-fn`: desde 100 KB de C, cada función va en
su propio archivo).

`DT_RELR` trae 222.402 relocaciones relativas en `main` (198.097 apuntan a código), pero casi todas a
funciones que `gen.py` ya encontraba por `.eh_frame`, `bl` o `adrp` + `add`: suma 2 inicios de función
en `main`, 17 en `subsdk0` y 8 en `sdk`. `runtime/hle/hle_symbols.txt` solo lista `longjmp` (del `sdk`).

Compilación (`make -j12`, con `build/savagewind.exe` ya hecho, que tarda unos 10 s):

- 19 min 1 s (1141 s) para los 608 archivos más el enlace. El enlace (lld) tarda 0,5 s y usa ~0,3 GB.
- Memoria pico: 2,9 GB sumando los 12 clang que corren a la vez; el clang más grande llegó a 731 MB.
- Objetos: 250,6 MB (`main` 188,3 MB, `subsdk0` 20,0 MB, `sdk` 42,3 MB).
- `build/swgame.dll`: 198,8 MB (`.text` 179,2 MB, `.rdata` 10,8 MB, `.pdata` 2,3 MB). Exporta solo
  `swgame_tables` e importa de `savagewind.exe` y del CRT.
- Una primera pasada con las direcciones fijas de antes (`main` y `sdk`, sin `subsdk0`) tardó casi lo
  mismo: 1171 s para 509 archivos y una DLL de 182,3 MB.

El tiempo de compilación no depende del volumen de C sino de unas pocas funciones. Cuando una función
tiene un `br` por tabla (resuelta o no), `emit_function` le pone una etiqueta a cada instrucción y un
`switch` en `L_dispatch` con un `case` por instrucción, por si la tabla emulada no cubrió algún caso.
Con 8.000 a 12.000 casos, clang -O1 se vuelve muy lento:

- Compilando 6 a la vez, `main_104.c` (una función de 1,08 MB de C, 8.089 etiquetas) tarda 553 s (9 min).
  `main_248.c`, más grande (1,7 MB) pero con 2 etiquetas, tarda 31 s.
- De los 145 archivos de una sola función de `main` y `sdk` (4.099 s de CPU, 6 a la vez), los 66 que
  tienen ese `switch` suman 3.972 s (97 %), y los 20 más lentos, 3.238 s.
- Compilados solos: un archivo normal de 1,5 MB con 535 funciones tarda 4,9 s; la tabla de `main`
  (8 MB), 2,3 s.

Bajar ese `switch` (por ejemplo, a solo las instrucciones que pueden ser destino, o despachando por
`rt_dispatch`) es lo que más acortaría la compilación, pero cambia lo que hace el código generado cuando
una tabla no se resolvió bien. (Los tiempos varían mucho con la carga: `main_248.c` tarda 10,5 s sola y
31 s con otros 5 compilando.)

Después de integrar `module-layout`, `gamelib-build` y `jump-tables` en `main` (misma máquina,
2026-10-14): `make gen` tarda 50 s (`main` 122.961 funciones en 428 archivos, `subsdk0` 13.934 en 143,
`sdk` 40.461 en 205) y `make -j12` 969 s (16 min 9 s) sin errores, con un `build/swgame.dll` de
211,7 MB. Una corrida con `savagewind.exe extracted --play` reubica los tres módulos, corre los
constructores del `sdk` y se detiene donde dice "Pendiente" 2: mensajes TIPC 16 y 17 a `sm:` y
`svcBreak(0)` en 0x700251863c.

Arreglos en `tools/sw_probe.py` de esta misma tanda:

- Las importaciones se agrupan por namespace (`nn::os` 85, `nn::audio` 74, `nn::util` 34, ...). Antes,
  402 de las 465 de `main` caían en un grupo vacío porque no se saltaba el prefijo `_ZN`.
- Las funciones NVN que enlaza `runtime/hle` (`nvnBootstrapLoader`, `nvnDeviceGetProcAddress`) cuentan
  como implementadas. Con 1.8.2: el juego nombra 569, el runtime implementa 515 (86 vacías) y faltan 54.
