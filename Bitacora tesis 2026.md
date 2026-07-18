* 18/1  
  * Encontre unos links a unos demos que van a servir mucho estos son:  
    * [https://digilent.com/reference/programmable-logic/arty-z7/demos/start](https://digilent.com/reference/programmable-logic/arty-z7/demos/start)  
  * Voy a analizar a ver que onda el diseño del output:  
    * [https://docs.google.com/document/d/1j4xGe8hiio-TsuYZm6inOciyUy2EwFRI\_vDNohmA3jQ/edit?tab=t.pooyy767m03](https://docs.google.com/document/d/1j4xGe8hiio-TsuYZm6inOciyUy2EwFRI_vDNohmA3jQ/edit?tab=t.pooyy767m03)  
  * El de in es input \+ output asi que servirá de base para el proyecto  
    * [https://github.com/Digilent/Arty-Z7-20-hdmi-in/releases/tag/v2018.2-2](https://github.com/Digilent/Arty-Z7-20-hdmi-in/releases/tag/v2018.2-2)  
* 22/2  
  * Como esto no anduvo de una, vamos a hacer el input output pero a mano y de una  
* 28/2  
  * No vi cambios relevantes al hacer a mano el input, asi que se realiza el que esta pero con algunos ajustes de señales con error y asociar bien la placa  
* 3/3  
  * Modifique el codigo para agregar prints  
  * Viendo el codigo de demo vi que hay que ajustar la resolucion primero:  
    	Before plugging in the HDMI source  
    5\. Critical step — match the display resolution to your laptop's output.  
    * Your laptop probably outputs 1080p or 720p. The display defaults to 640x480. For passthrough, the display resolution needs to match the capture resolution, otherwise you'll only see a corner of the image.  
      In the UART menu, press 1 and then set the resolution to match what your laptop will send:  
      Press 5 for 1920x1080 if your laptop is 1080p  
      Press 3 for 1280x720 if your laptop is 720p  
      Watch for \[INFO\] Display resolution: 1920x1080@60Hz to confirm.  
* 6/3  
  * Bueno bien se ve algo en la pantalla\!  
  * Se logro haciendo muchos cambios en el codigo pero se pudo  
  * Hay que subirlo a un repo para no cagarla ahora. Asi que dejamos por ahi.  
* 22/3  
  * Bueno hice un nuevo IP en el IP generator, tengo que rehacerlo pero con el wizard porque creo que quedo medio fulero armado, vamos a rehacerlo bien.  
  * Los problemas de clk no parecen ser tan relevantes  
* 4/4  
  * Tenemos andando los subtitulos lo mas bien ahora abria que cargarle el peta linux y hacerlo andar con peta linux  
  * El plan siguiente seria:  
    * Documentar que hace cada bloque y las decisiones que tomamos para cada bloque.  
    * Hacer los cálculos del buffering  
* 25/4  
  * Ya tengo el setup para poder buildear, y tengo que debugear la build que hice para pasar del IDE a LINUX  
  * Tengo que buildear desde vm y pasarlo a windows y de windows a la board  
  * Me acuerdo que tuve que editar el device tree aca para poder deshabilitar los hw on boot y hacerlo en sw  
    * home/tesislinux/tesis/hdmi-overlay/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi  
  * Con este commando configure el linux para incluir diversos paquetes como ssh vim curl wget etc  
    * petalinux-config \-c rootfs  
  * Para cosas de sw que quiero que tengan permanencia edite:  
    * mkdir \-p project-spec/meta-user/recipes-core/images/files/etc/init.d   
    * mkdir \-p project-spec/meta-user/recipes-core/images/files/root/.ssh  
  * Asi fue como lo logre:

  your SSH public key

  a boot script that gives the board the fixed IP

  Use these exact steps in the VM.


  1\. Put your SSH key into authorized\_keys


  First, on Windows PowerShell, print your board key:


  Get-Content $HOME\\.ssh\\id\_rsa\_board.pub

  Copy that whole one-line output.


  Then in the VM, open this file:


  nano \~/tesis/hdmi-overlay/project-spec/meta-user/recipes-core/images/files/root/.ssh/authorized\_keys

  Paste the whole public key line into it.


  Save with:


  Ctrl+O

  Enter

  Ctrl+X

  Then set permissions:


  chmod 700 \~/tesis/hdmi-overlay/project-spec/meta-user/recipes-core/images/files/root/.ssh

  chmod 600 \~/tesis/hdmi-overlay/project-spec/meta-user/recipes-core/images/files/root/.ssh/authorized\_keys

  2\. Create the boot-time IP script


  Open this file in the VM:


  nano \~/tesis/hdmi-overlay/project-spec/meta-user/recipes-core/images/files/etc/init.d/S99board-net

  Put this exact content in it:


  \#\!/bin/sh

  ip addr add 192.168.1.10/24 dev eth0 2\>/dev/null

  ip link set eth0 up

  exit 0

  Save it, then make it executable:


  chmod \+x \~/tesis/hdmi-overlay/project-spec/meta-user/recipes-core/images/files/etc/init.d/S99board-net

  3\. Rebuild the image


  From the VM:


  cd \~/tesis/hdmi-overlay

  source /home/tesislinux/tesis/settings.sh

  petalinux-build

  petalinux-package \--boot \--fsbl images/linux/zynq\_fsbl.elf \--fpga images/linux/system.bit \--u-boot

  4\. Copy the new boot files to the SD card


  After the build, the files to use are:


  \~/tesis/hdmi-overlay/images/linux/BOOT.BIN

  \~/tesis/hdmi-overlay/images/linux/image.ub

  Replace the old ones on the SD card with those.

* 4/26  
  * Hoy voy a probar si lo de la permanencia anda bien  
* 5/1  
  * No anduvo lo de la permanencia parece ser que el petalinux is generating bootargs with no root device  
    *   
  * Funciono ahora si arreglarlo cambiando manualmente los bootargs:  
    * Tenemos que settearle esto en Kernel Bootargs  
    * console=ttyPS0,115200 earlyprintk root=/dev/ram0 rw  
  * Ahora no anduvo directamente lo del ssh  
  * Esto fue lo que intentamos hacer:  
    So S99board-net means: “during system startup, near the end, run this board network script.”  
    * Pero no funciono ahora vy a ver de editar el posta sin el user overlay  
  * Bueno pero no anduvo nada ahora lo que hicimos fue crear esas files pero en el overlay que se supone que es para eso tambien tuvimos que poner un siymlink para que lo agarrase:

cd \~/tesis/hdmi-overlay

mkdir \-p project-spec/meta-user/rootfs-overlay/root/.ssh

mkdir \-p project-spec/meta-user/rootfs-overlay/etc/init.d

mkdir \-p project-spec/meta-user/rootfs-overlay/etc/rcS.d

cp project-spec/meta-user/recipes-core/images/files/root/.ssh/authorized\_keys \\

   project-spec/meta-user/rootfs-overlay/root/.ssh/authorized\_keys

cp project-spec/meta-user/recipes-core/images/files/etc/init.d/S99board-net \\

   project-spec/meta-user/rootfs-overlay/etc/init.d/S99board-net

ln \-sf ../init.d/S99board-net \\

   project-spec/meta-user/rootfs-overlay/etc/rcS.d/S99board-net

chmod 700 project-spec/meta-user/rootfs-overlay/root/.ssh

chmod 600 project-spec/meta-user/rootfs-overlay/root/.ssh/authorized\_keys

chmod 755 project-spec/meta-user/rootfs-overlay/etc/init.d/S99board-net

* Okay entonces estan las recipies que compilan el codigo de yocto/bitbake y son las .bb  
  * Ahora lo que vamos a hacer es usar un append que no es la receta completa sino un agregado a lo que ya este  
    * A .bb recipe is a build instruction for Yocto/BitBake. It answers: “how do I turn these source files into something installable into the target root filesystem?”  
      * A .bbappend is not a full recipe. It is a patch on top of an existing recipe. It answers: “take that recipe that already exists, and add or change a few things.”  
* 16/5  
  * Solucionamos los problemas de output que estábamos teniendo\!  
    * El problema es que confie demaciado en el output de los drivers que habian sido generados este termino siendo el problema

Great, output is back. That pretty much confirms the Linux VTC polarity emulation was the blocker: v\_axi4s\_vid\_out was not locking/accepting AXIS because the generated VTC polarity register did not match bare-metal.  
Summary for your log:

* Fixed Linux VTC generator polarity programming in linux\_code/src/drivers/xvtc.c.  
* Added missing Xilinx BSP-compatible VTC polarity masks in linux\_code/src/drivers/xvtc.h:  
  * field ID polarity  
  * active chroma polarity  
  * active video polarity  
  * full polarity mask  
* Updated XVtc\_SetGeneratorTiming() so GPOL matches the Xilinx BSP behavior:  
  * always sets FieldIdPol  
  * always sets ActiveChromaPol  
  * always sets ActiveVideoPol  
  * sets HBlankPol/HSyncPol from mode H polarity  
  * sets VBlankPol/VSyncPol from mode V polarity  
* Added missing XVtc\_SourceSelect fields for closer BSP parity:  
  * FieldIdPolSrc  
  * InterlacedMode  
* Updated XVtc\_SetSource() to program those fields into the VTC control register.  
* Added VTC0 generator register dumps in linux\_code/src/display\_ctrl/display\_ctrl.c:  
  * after VTC generator enable  
  * after VDMA start/parking  
* Added linux\_code/baremetal\_vtc\_dump\_snippet.c as a reference-only SDK snippet for comparing bare-metal VTC register values without modifying SDK files.

Root cause: the Linux userspace VTC shim was incomplete. It programmed timing sizes plausibly, but did not set the active-video/chroma/field-id polarity bits that the Xilinx bare-metal BSP sets. That left v\_axi4s\_vid\_out unable to lock to the VTC timing, so it held AXIS TREADY=0, stalling VDMA on the first pixel.

* 23/5  
  1. Cree un nuevo repositorio y empece a hacer el setup  
  2. Importe los clang and stuff  
  3. Ahora lo que estoy haciendo es agarrar los modulos y ver cuales ya tienen drivers de linux y cuales no, tengo que ver bien eso  
  4. Creo una lista de cuales van a usar que drivers si los drivers de linux, los drivers de xilinx linux, los drivers de Diligent linux y/o los drivers de BSP usando MMIO o UIO.  
* 24/5  
  1. Voy a crear un nuevo item en el device tree para hacer un dma client desde el kernel asi puede mejor coordinar las cosas el kernel  
  2. Para hacer esto se hace  
     * Se edita el device tree declarando el nuevo componente  
     * Habilitamos dma para que lo use el kernel de linux y tambien creamos el nuevo componente  
     * Comenzamos con la creacion del nuevo customer del kernel  
       * petalinux-create \-t modules \--name hdmi-mm2s-client \--enable  
* 13/6 - 16/6 (esta nota habia quedado anotada fuera de orden)  
  1. final working USB audio setup:  
  2.   
  3. USB audio adapter behind the USB 2.0 hub.  
  4. CONFIG\_USB\_EHCI\_TT\_NEWSCHED=y.  
  5. Native ALSA capture through hw:0,0.  
  6. 48000 Hz, mono, S16\_LE, 20 ms chunks.  
  7. Why the old failure happened: full-speed USB isochronous audio needed proper EHCI transaction-translator scheduling; otherwise it enumerated but produced no audio chunks / error \-28: not enough bandwidth.  
* 

## Actualizacion reconstruida desde el historial del repo

18/5 \- 31/5

Se arranco el repo nuevo de firmware y se separo mejor lo que antes estaba mezclado en el proyecto de referencia.

Hitos:

\- 18/5: Initial commit.

\- 23/5: se agrego clang/clang-tidy, Coverity y BSP base.

\- 29/5: se reubico el BSP y se empezo a ordenar la estructura del repo.

\- 30/5: se agregaron utils y el main app.

\- 31/5: se crearon app.c/app.h, templates y primeros Active Objects.

Decision importante:

El repo activo de trabajo pasa a ser este:

/home/nacho/subtitle\_overlay\_fw

El proyecto viejo de Windows queda como referencia, no como fuente principal.

Arquitectura que fue quedando:

\- src/app: inicializacion QF/QP y arranque de AOs.

\- src/svc: servicios por dominio con Active Objects.

\- src/hal: acceso a hardware y SO (VDMA, VTC, USB audio, BRAM, overlay).

\- src/bsp: drivers BSP de Xilinx que necesitamos portar o adaptar.

\- test: mirror de src para Ceedling/Unity/CMock.

31/5 \- 4/6

Se empezo a portar el pipeline de video/control a una arquitectura de servicios.

Commits relevantes:

\- define new AO

\- add new AO

\- device tree

\- hdmi\_vdma\_client

\- import bsp compat and make service AO

\- implement the virtual mapping

\- Implement systemAO and app

Milestone:

El firmware deja de ser un programa lineal estilo demo bare-metal y empieza a tener una arquitectura por Active Objects:

\- SystemAO como supervisor.

\- VideoAO / video\_pipeline para inicializar y manejar VDMA/VTC.

\- SubtitleAO despues para el overlay.

\- SttAO y USBAudioAO despues para audio/STT.

Tambien se empezo a consolidar la frontera kernel/userspace del VDMA:

\- linux/hdmi\_vdma\_client

\- device tree con nodo para el cliente

\- idea: que el kernel coordine el DMA y userspace no toque todo de forma peligrosa.

4/6

Se agregaron unit tests con Ceedling/Unity.

Commits:

\- Add unit testing

\- move tests to correct folder

\- add error numbers

\- change state machine

\- adjust Video AO state machine

Importante:

Los tests pasan a ser parte del flujo normal. Desde aca el proyecto empieza a tener:

\- make test

\- make coverage

\- make clang-tidy

\- mocks de HAL con CMock

\- convencion de errores negativos tipo \-EINVAL/-EIO

4/6 \- PR \#1 subtitle\_pipeline

Merge pull request \#1 from Nacholazabal/subtitle\_pipeline

Titulo: Add subtitle pipeline

Este fue el primer PR grande del pipeline de subtitulos.

Se agrego:

\- src/hal/subtitle\_bram

\- src/hal/subtitle\_overlay

\- src/svc/subtitle\_pipeline/SubtitleAO

\- src/svc/subtitle\_pipeline/subtitle\_pipeline

\- tests de BRAM, overlay y subtitle pipeline

Que resolvio:

Se creo el camino software para escribir el bitmap del subtitulo hacia BRAM/overlay y controlarlo desde un AO.

La idea base quedo asi:

STT/texto \-\> SubtitleAO \-\> renderer/pipeline \-\> subtitle\_bram \-\> overlay HDMI.

4/6 \- PR \#2 add\_logs

Merge pull request \#2 from Nacholazabal/dev/add\_logs

Se agrego logging al firmware:

\- app

\- SystemAO

\- SubtitleAO

\- VideoAO

Esto fue importante porque hasta ese momento era muy dificil saber donde quedaba trabado el sistema.

Desde aca empezamos a tener trazas de inicializacion y errores por componente.

4/6 \- PR \#3 audit\_fixes

Merge pull request \#3 from Nacholazabal/dev/audit\_fixes

Se aplicaron fixes de auditoria/review:

\- app.c/app.h

\- SubtitleAO

\- SystemAO

\- VideoAO

Mejoras principales:

\- estado de inicializacion mas robusto.

\- reporting de errores mas consistente.

\- menos comportamiento implicito entre AOs.

4/6 \- 5/6

Infraestructura de build/CI.

PR \#4: fix build scripts

\- se renombro scripts/build\_on\_vm.sh a scripts/build.sh

\- se arreglo run\_on\_board.sh

PR \#5: fix coverage ceedling invocation

\- se corrigio CI para coverage.

\- make coverage / scripts/coverage.sh quedan mejor integrados.

Tambien se agrego el submodulo QP/C:

\- src/qpc como framework QP/C vendorizado/submodule.

Decision:

Para build real de ARM no se compila local en WSL: se usa la VM PetaLinux con scripts/build.sh.

Para tests/lint se usa host:

\- make test

\- make clang-tidy

14/6 \- PR \#6 audio\_pipeline

Merge pull request \#6 from Nacholazabal/dev/audio\_pipeline

Este fue uno de los hitos mas grandes del proyecto.

Se agrego casi todo el camino de audio y STT:

\- docs/linux/usb\_audio\_pipeline.md

\- linux/kernel/usb\_audio\_host.cfg

\- scripts/audio\_receiver.py

\- HAL USB audio: src/hal/usb\_audio/usb\_audio\_capture.c

\- USBAudioAO

\- usb\_audio\_stream

\- SttAO

\- stt\_event\_rx

\- subtitle\_text\_renderer

\- tests de usb\_audio, stt\_event\_rx, audio\_receiver

Arquitectura que quedo:

Board captura audio USB con ALSA \-\> firmware manda PCM por TCP \-\> receiver en PC/STT procesa \-\> PC devuelve eventos NDJSON \-\> SttAO recibe \-\> SubtitleAO renderiza \-\> overlay.

Formato/protocolo:

\- Audio PCM S16\_LE, 48 kHz, mono.

\- Chunks de 20 ms.

\- TCP de audio hacia PC.

\- NDJSON de transcript de vuelta hacia la board.

\- stt\_event\_rx parsea seq, type/is\_final, start/end y text.

Leccion importante:

ALSA es el camino normal. No conviene agregar knobs para deshabilitar ALSA como solucion "facil"; si falta alsa/asoundlib.h el problema es el sysroot/build environment.

14/6 \- PR \#8 subtitle\_box

Merge pull request \#8 from Nacholazabal/dev/subtitle\_box

Se ajusto el render del subtitulo:

\- subtitle\_pipeline.c

\- subtitle\_text\_renderer.c

\- tests

Objetivo:

Mejorar la caja/bitmap de subtitulo para que el texto quede mejor presentado en pantalla.

Este PR fue chico en lineas, pero importante para la UX visual.

14/6 \- PR \#7 stt

Merge pull request \#7 from Nacholazabal/dev/stt

Se agrego el receptor STT en Python para Windows/PC:

\- requirements-stt.txt

\- scripts/run\_stt\_windows.sh

\- scripts/stt\_receiver.py

Funcion:

El firmware manda audio a la PC; el script corre faster-whisper y devuelve eventos de subtitulos a la board.

Este fue el primer loop completo util para probar STT sin meter inferencia pesada en el ARM.

19/6 \- PR \#9 improve\_timing

Merge pull request \#9 from Nacholazabal/dev/improve\_timing

Hito de tuning y observabilidad.

Se agrego:

\- scripts/analyze\_noise.py

\- scripts/analyze\_noise\_spectrum.py

\- scripts/analyze\_run.py

\- scripts/subtitle\_debug.py

\- scripts/reference\_es.txt

\- skill subtitle-review

\- subtitle\_text\_sanitize

\- usb\_audio\_agc

\- mejoras grandes en stt\_receiver.py

\- tests de sanitize y AGC

Que aprendimos/probamos:

\- Analizar una run completa es mas util que mirar logs sueltos.

\- analyze\_run.py resume audio, eventos, timing, transcript y posibles problemas.

\- subtitle\_debug.py permite reproducir logs de eventos sin tener la board.

\- sanitize limpia texto antes de renderizar para evitar basura visual.

\- usb\_audio\_agc normaliza niveles, pero despues descubrimos que puede afectar el VAD si se usa mal.

Algoritmos/ideas que quedaron:

\- Auto-normalizacion por peak/running peak.

\- Filtro de alucinaciones comunes de Whisper en silencio o ruido.

\- Segmentacion por VAD/silencio y max\_window.

\- Partials opcionales.

\- Finales enviados al firmware como eventos estables.

\- Sanitizado de texto y render mas controlado.

19/6 \- 28/6

Hubo una limpieza bastante grande del repo despues del PR \#9.

Que se hizo de verdad en estos commits:

\- Se ordenaron contratos y errores de HAL/servicios.

\- Se refactorizo el parser de eventos STT y el stream de audio.

\- Se agregaron tests para log, number\_parse, STT y USB audio.

\- Se borraron utils que habian quedado sin uso (ring buffer, timestamp, log2fix y delay viejo).

\- Se sacaron documentos y configuraciones viejas que ya no representaban el proyecto.

\- QP/C dejo de estar declarado como submodulo y quedo vendorizado dentro de src/qpc.

Leccion: el commit `cleanup` del 28/6 fue principalmente una poda de material viejo. La wiki, AGENTS.md y las skills que usamos ahora son documentacion de apoyo posterior; no corresponde atribuirlas todas a ese commit.

3/7 \- Experimento Colab GPU

Se documento en docs/performance\_comparison\_cpu\_vs\_gpu.md.

Antes:

\- STT local en CPU con faster-whisper small, int8, threads=2.

\- Inferencia por chunk estimada: 0.5s \- 1.5s.

\- Gap entre finales: 1s \- 2s.

\- End-to-end perceptual: 2s \- 4s.

\- Drops frecuentes cuando la CPU no daba abasto.

Despues:

\- Google Colab T4 GPU gratis.

\- faster-whisper small, CUDA float16.

\- PC \<-\> Colab via ngrok.

\- Mismo loop board \-\> PC \-\> Colab \-\> PC \-\> board.

Medicion del 2026-07-03:

\- Duracion: 115.0s.

\- Finals: 24\.

\- Partials: 0\.

\- Hallucinations: 0\.

\- Audio drops: 0\.

\- Gap maximo: 0.50s.

\- Gap promedio: 0.02s.

\- Latencia perceptual estimada: 0.5s \- 1.0s.

Conclusion:

La inferencia dejo de ser el cuello de botella.

Colab GPU cumplio la meta de latencia de la tesis (\<1.5s perceptual) para desarrollo/testing.

Quedan como cuellos:

\- red PC-Colab si se quiere bajar mas.

\- VAD/segmentacion.

\- timing/UX del overlay.

5/7 \- PR \#10 increase\_testing

Merge pull request \#10 from Nacholazabal/dev/increase\_testing

Hito de research \+ Colab \+ integracion.

Se agrego:

\- deep-research-report.md

\- docs/colab\_inference\_setup.md

\- docs/performance\_comparison\_cpu\_vs\_gpu.md

\- scripts/colab\_inference\_server.ipynb

\- scripts/run\_stt\_colab.sh

\- mejoras grandes en stt\_receiver.py

\- test/integration/qpc/test\_stt\_to\_subtitle\_flow.c

\- test/integration/qpc/test\_system\_ao\_startup.c

\- test/support/qpc\_test\_harness

\- tests de renderer y stt\_receiver

Importante de arquitectura:

Se agregaron tests de integracion QP/C para probar flujo real entre AOs:

\- SystemAO startup.

\- SttAO \-\> SubtitleAO.

\- parciales/finales.

\- deduplicacion por secuencia.

\- errores de poll STT.

Importante de algoritmo:

El deep research recomienda no pensar el subtitulo como "reescribir texto entero todo el tiempo".

El patron bueno es:

\- buffer comprometido/inmutable.

\- cola parcial mutable.

\- promover texto a final cuando hay estabilidad.

\- evitar flicker de parciales.

\- mantener un buffer de audio acotado y recortarlo segun commits/timestamps cuando sea posible.

Settings Colab documentados:

\- STT\_MAX\_WINDOW\_SEC=4.0

\- STT\_MIN\_SILENCE\_SEC=0.5

\- STT\_PARTIAL\_SEC=0.8 en setup Colab, aunque la comparacion final buena se hizo con partials deshabilitados.

\- STT\_PARTIAL\_AGREEMENT=2

\- STT\_GAIN=0 auto-normalizacion

\- STT\_BEAM\_SIZE=5

Despues del merge, `87f42c1` ajusto `analyze_run.py` y `stt_receiver.py` para que las mediciones y el filtrado de eventos fueran mas confiables.

5/7 \- Streaming STT por WebSocket (inicio de la rama en `6d5becf`)

Rama abierta en ese momento: dev/VAD-improvements

Despues del PR \#10 se empezo una ruta nueva para reducir overhead HTTP/Colab:

Board \-\> PC bridge \-\> Colab/VPS WebSocket STT server \-\> PC bridge \-\> Board

Archivos nuevos/importantes:

\- docs/streaming\_stt\_setup.md

\- docs/streaming\_stt\_run\_log.md

\- scripts/colab\_streaming\_server.ipynb

\- scripts/run\_stt\_colab\_stream.sh

\- scripts/stt\_stream\_bridge.py

\- scripts/stt\_stream\_protocol.py

\- scripts/stt\_stream\_server.py

\- scripts/analyze

\- mejoras en analyze\_run.py

Objetivo:

Pasar de requests HTTP por chunk a una sesion WebSocket full-duplex, mas parecida al protocolo final.

La PC sigue como bridge de debug, pero la arquitectura apunta a:

Board \-\> wss://server/stt/stream \-\> Board

Defaults streaming iniciales:

\- model=small

\- max\_window=3.0s

\- min\_silence=0.35s / notebook reportando 0.30s en algunas runs

\- partial=0.5s

\- partial\_agreement=1

\- beam=5

\- gain=0.0

\- vad\_filter=True

Despues se probo volver a settings mas estables:

\- max\_window=4.0s

\- min\_silence=0.5s

\- partial=0.8s

\- partial\_agreement=2

Runs streaming documentadas

S001 \- 2026-07-05 17:42 \- Streaming defaults

\- 115.2s.

\- 145 eventos: 31 finals, 114 partials.

\- Todos los finals por max\_window=3.0s.

\- GPU bien: p50 0.20s, p90 0.34s.

\- Server queue casi cero.

\- Problema: segmentacion y churn visual.

\- Audio con clipping 0.044%.

\- Muchos updates duraban menos de 1.5s.

Conclusion S001:

El cuello no era GPU/WebSocket. El problema era que el VAD no cortaba por silencio y el max\_window cortaba artificialmente.

S002 \- 2026-07-05 17:52 \- Streaming defaults nueva muestra

\- 67.0s.

\- 90 eventos: 21 finals, 69 partials.

\- 20/21 finals por max\_window, 1 por silence.

\- p10 del noise floor subio a \-22.1 dBFS, rango dinamico solo 7.8 dB.

\- Se ve que el "silencio" no era realmente silencio para VAD.

\- Churn visual alto: p50 display spacing 0.52s.

Conclusion S002:

La run no aplico los knobs mas estables; seguia midiendo perfil agresivo.

Ademas el ruido de entrada hizo que VAD tuviera poca separacion entre voz y silencio.

S003 \- 2026-07-05 17:59 \- Calidad muy mala

\- 72.0s.

\- 106 eventos: 22 finals, 84 partials.

\- 22/22 finals por max\_window.

\- p10=-21.7 dBFS, rango 6.8 dB.

\- Transcript muy malo.

Conclusion S003:

No habia evidencia de regresion de defaults contra commits tempranos. El candidato principal era audio/captura/AGC \+ segmentacion sin silencios, no el modelo.

S004 \- 2026-07-05 18:18 \- Knobs 4.0/0.5/0.8/2

\- 93.5s.

\- 65 eventos: 24 finals, 41 partials.

\- 21 max\_window, 3 silence.

\- Dropped audio jobs bajo a 1\.

\- Server queue max 0\.

\- Finals bajo 1.5s bajaron mucho: 5/23.

\- Mejor cadencia visual.

\- Costo: primer parcial visible mas tarde, cerca de 1.6s-2.4s.

Conclusion S004:

partial\_sec=0.8 \+ agreement=2 mejora UX estable, pero puede sentirse lento al inicio.

VAD sigue cap-limited por falta de contraste en audio.

S005 \- 2026-07-05 18:41 \- Board AGC off, Colab auto gain

\- 70.7s.

\- 38 eventos: 15 finals, 23 partials.

\- 15/15 finals por max\_window.

\- Dropped=0.

\- p10=-63.8 dBFS y rango 49.0 dB: mejora enorme respecto a S002/S003/S004.

\- Transcript vuelve a ser mayormente entendible.

\- Ningun final duro menos de 1.5s visible.

Decision S005:

Default nuevo: correr sin AGC digital en la board.

La board debe mandar PCM crudo por default y dejar que Colab/server haga normalizacion durante tuning.

Esto corresponde a commits:

\- 8d4ed79 new gain

\- a59938f default to no gain

S006 \- 2026-07-05 19:10 \- Football background, board AGC off

\- 117.3s.

\- 73 eventos: 33 finals, 40 partials.

\- 17 max\_window, 16 silence.

\- Peak 73.3%, RMS \-20.4 dBFS.

\- Noise floor alto: p10=-25.8 dBFS, rango 7.5 dB.

\- GPU bien: p90 0.31s.

\- Server queue p90 0\.

Conclusion S006:

Con futbol de fondo el caso es mas dificil, pero Silero si detecto silencios casi la mitad de las veces.

El problema pasa a ser segmentacion bajo ruido \+ politica visual para que no se reemplacen textos demasiado rapido.

Tracing / instrumentacion agregada

Se agregaron campos y analisis para separar latencia por etapa:

\- audio buffer

\- server queue

\- GPU infer

\- server emit lag

\- bridge recv lag

Y metricas VAD:

\- vad\_segment\_count

\- vad\_speech\_ratio

\- vad\_last\_speech\_end\_sec

\- trailing\_silence\_sec

\- window\_rms\_dbfs

\- tail\_rms\_dbfs

\- conteo finals por silence vs max\_window

Esto es clave para no seguir tuneando a ciegas.

Ahora podemos saber si:

\- Silero ve todo como speech continuo.

\- Silero ve pausas pero no llegan a min\_silence\_sec.

\- El audio tiene piso demasiado alto.

\- La latencia viene de inference, queue, red o buffer de audio.

Estado de settings que parecia mejor al 5/7 (despues fue refinado por el sweep S008)

Para streaming/Colab:

\- model=small

\- beam=5

\- board digital AGC off

\- server gain=0.0 auto-normalizacion

\- max\_window=4.0s

\- min\_silence=0.5s

\- partial=0.8s

\- partial\_agreement=2

\- vad\_filter=True

Tradeoff:

\- partial=0.5/agreement=1 se siente mas live pero genera mucho flicker/churn.

\- partial=0.8/agreement=2 es mas estable y legible, pero tarda mas en mostrar primer parcial.

\- finals-only en Colab HTTP fue muy bueno en latencia medida, pero streaming con parciales permite sensacion mas live si se controla el churn.

Pendientes anotados al 5/7

\- Instrumentar y usar VAD en cada run antes de cambiar modelo.

\- Probar threshold VAD mas alto si Silero marca ruido como speech.

\- Probar min\_silence=0.35 con max\_window=4.0 si vemos pausas reales cortas.

\- Agregar politica de display hold/coalescing: no reemplazar texto visible si duro menos de \~1.0s-1.5s, especialmente parciales.

\- Comparar small vs medium/base solo despues de estabilizar audio/VAD.

\- Medir latencia end-to-end con timestamps precisos desde audio capturado hasta pixel visible.

\- Eventualmente eliminar el bridge PC y hacer cliente WebSocket TLS directo desde firmware/board.

## Continuacion revisada contra Git

* 6/7  
  * Corri la S007 con futbol de fondo y la instrumentacion nueva de VAD.  
  * Esta run fue bastante util porque por fin se pudo ver que el VAD no estaba simplemente "roto": 33 de 44 finales salieron por silencio y 11 por max window.  
  * La mediana de la ventana final bajo a 1.41 s y la GPU siguio rapida (p90 de inferencia de 0.28 s).  
  * El problema se movio a la UX: 154 de 165 updates duraron menos de 1.5 s. Con parciales cada 0.5 s el texto cambiaba demasiado rapido.  
  * Proximo perfil a probar: `max_window=3.0`, `min_silence=0.3`, `partial=0.7` y `partial_agreement=2`.

* 13/7 - banco de pruebas corto (`e0fe113`)  
  * Arme `scripts/audio_test_short.py` para dejar de evaluar todo a ojo con runs largas.  
  * El script reproduce audios cortos conocidos, guarda audio/eventos, reconstruye cuanto tiempo quedo visible cada subtitulo y genera un reporte.  
  * Tambien se agregaron pruebas del protocolo streaming y fixtures de audio para poder repetir los mismos casos.  
  * La idea es separar cuatro cosas que antes se mezclaban: exactitud, latencia, legibilidad y confiabilidad del transporte.

* 14/7 - PR \#11 a `main` (`431700b`)  
  * Se mergeo `dev/process_all_in_colab`. Con esto `main` ya tiene el servidor WebSocket de streaming, el bridge PC, el protocolo, el notebook de Colab, el analizador y las primeras seis runs documentadas.  
  * Tambien entro el cambio para que la board mande PCM crudo por default y la ganancia se haga del lado del servidor durante el tuning.  
  * Este PR consolida el camino Board -> PC bridge -> Colab -> PC -> Board. La PC sigue siendo un puente de debug; todavia no hay cliente WebSocket/TLS directo en el firmware.

* 14/7 - automatizacion de pruebas en `dev/VAD-improvements`  
  * Entre `a16ace1`, `4259430`, `1118b5d` y `6894d59` fui ajustando el runner, el notebook y la forma de levantar/copiar el servidor en Colab/Google Drive. Fueron iteraciones chicas para que la prueba sea repetible y no dependa de editar celdas a mano cada vez.  
  * En `7593b12` agregue el sweep automatico de parametros. Ahora una sola corrida puede comparar varias combinaciones de ventana, silencio, parciales y thresholds VAD y dejar un reporte agregado.  
  * Primer sweep S008 (tres audios cortos, Whisper small, beam 5):
    * `wider_context` (`window=4.0`, `silence=0.5`, `partial=1.0`, `agreement=2`) dio el mejor resultado de exactitud: WER proxy 23.52%, CER proxy 15.57%, p90 0.93 s y cero candidatos a alucinacion.
    * Usar `partial=1.0` y `agreement=2` mejoro mucho la legibilidad frente al baseline de 0.5 s.
    * Cambiar solamente los thresholds VAD no dio una mejora concluyente.
  * Ojo con este resultado: la referencia era la transcripcion offline del mismo modelo, no una referencia humana. Sirve para comparar configuraciones, pero todavia no es una medicion final de WER/CER.

* 14/7 - encontramos un agujero en la medicion y lo corregimos (`30b59f9`)  
  * En el sweep, solo la primera corrida habia llegado realmente a la pantalla. Las siguientes reiniciaban `seq=0`, mientras el firmware conservaba la secuencia anterior y rechazaba los eventos.  
  * Los reportes viejos decian que PC/Colab habia funcionado, pero no podian demostrar que la board hubiera aceptado cada subtitulo.  
  * Se agrego una sesion explicita en el canal NDJSON:
    * la board responde `session_ready` al conectar;
    * la secuencia se reinicia por conexion;
    * cada transcript recibe un `transcript_ack` con `accepted` o la causa del descarte;
    * el bridge espera el handshake y guarda los ACKs;
    * el reporte separa confiabilidad de audio/Colab de entrega aceptada por la placa.
  * Un ACK `accepted` prueba que SttAO recibio y encolo el evento hacia SubtitleAO. Todavia no prueba que los pixeles correctos hayan salido por HDMI; para eso falta captura/medicion fisica.
  * Tambien se dejo preparado un factorial 2x2 para comparar `window={3,4}` y `silence={0.3,0.5}`, con tres replicas intercaladas del control 4.0/0.5.

* 15/7 - mega review de `src/` (`f164004`)  
  * Se hizo una revision completa del firmware y se dejo el informe en `docs/reviews/src-mega-review-2026-07-15.md`.  
  * Importante: este commit agrega el diagnostico; no corrige todavia los hallazgos.  
  * Resultado de la review: 2 problemas criticos, 7 altos, 12 medios y 6 de deuda/baja severidad.  
  * Los cuatro riesgos para atacar primero son:
    * una carrera al arrancar el ticker POSIX-QV que puede dejar todos los timers sin ticks;
    * VideoAO puede desmontar MMIO que SubtitleAO todavia usa;
    * USBAudioAO hace operaciones bloqueantes dentro del scheduler cooperativo;
    * hay tres framebuffers declarados, pero no existe triple buffering real y puede haber tearing.
  * Lo positivo: 172 tests pasaron, los eventos/colas/pools estan acotados y la separacion AO/HAL da una base bastante buena. Lo mas flojo sigue estando en los bordes reales de threads, ALSA, MMIO y hardware, que los mocks no cubren.

* Estado al 16/7  
  * `main` llega hasta el PR \#11 (`431700b`).  
  * La rama actual es `dev/VAD-improvements` y tiene 9 commits de trabajo por encima de la base `a59938f`. El contenido de esos commits todavia no esta mergeado a `main`.  
  * En esta actualizacion conte solamente lo que esta commiteado. La bitacora, el deep research renombrado y otros documentos locales sin commit no los tome como hitos implementados.  
  * Siguiente paso tecnico razonable: resolver primero los criticos de la mega review y despues repetir el sweep con sesiones/ACKs activos, referencias humanas y evidencia del HDMI real.
