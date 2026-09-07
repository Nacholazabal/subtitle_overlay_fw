# Despliegue directo placa → Colab

## Objetivo

En producción, la Arty Z7 abre directamente una conexión WebSocket segura al
servidor Nemotron publicado desde Colab:

```text
PC ──SSH/SCP por LAN──► placa       (build, despliegue y diagnóstico)
placa ──WSS/443───────► ngrok ──► Colab/Nemotron  (audio y transcripts)
```

El PC no participa en el camino de audio durante la ejecución. Puede apagarse
después de instalar el ejecutable y su servicio. Colab sí debe permanecer
activo.

## Red

La placa y la PC deben conectarse al mismo router. No se usa el antiguo cable
Ethernet directo PC–placa.

La rootfs configura `eth0` mediante DHCP:

```text
auto eth0
iface eth0 inet dhcp
```

DHCP entrega a la placa una IP, gateway y DNS. La IP concreta no interviene en
la conexión a Colab porque ésta es saliente. Para desplegar, `scripts/run.sh`
localiza automáticamente la dirección actual mediante la MAC Ethernet
`00:0a:35:00:1e:53`; no es necesario modificar el alias SSH cuando cambia la
concesión. Una reserva DHCP sigue siendo opcional.

No se necesita port forwarding: la placa inicia tráfico saliente WSS por 443.

## Hora y TLS

La Arty Z7 no tiene RTC respaldado. La imagen incluye `ntp 4.2.8p10`, arranque
SysV `S20ntpd` y servidores `0.pool.ntp.org` a `3.pool.ntp.org` con `iburst`.
El cliente WSS espera una fecha plausible antes de intentar TLS. La rootfs
también incluye `/etc/ssl/certs/ca-certificates.crt`.

## Dos tipos de actualización

### Ejecutable del firmware

`subtitle_overlay_fw` es una aplicación Linux. El flujo habitual sigue siendo:

1. `./scripts/build.sh` la compila para ARM en la VM.
2. `scripts/run.sh` descubre la IP de la placa y copia el ejecutable por SCP a
   `/home/root/subtitle_overlay_fw`.
3. Instala o actualiza `/etc/init.d/subtitle-overlay` y reinicia el servicio.

La diferencia es solamente el recorrido de red: PC → router → placa. No hace
falta retirar la SD para actualizar este ejecutable.

Para instalar por primera vez o actualizar una instalación existente se usa el
mismo comando idempotente. El servicio persistente es siempre el único modo:

```bash
./scripts/run.sh
```

- Sin flags se buildea, detiene ordenadamente la instancia anterior, actualiza
  el ejecutable y su configuración, y vuelve a iniciar el servicio.
- `-p` realiza ese mismo flujo con tracing de firmware habilitado.
- `-x` omite el build y despliega el último artefacto local.
- La hora se obtiene de NTP desde la imagen de producción; el script de deploy
  no modifica manualmente el reloj de la placa.
- Si la detección automática no está disponible, `BOARD_IP=x.x.x.x` permite
  indicar la dirección explícitamente para esa ejecución.

### Imagen PetaLinux

Cambios de kernel, device tree, rootfs, DHCP o NTP requieren regenerar y grabar
la SD. En la VM:

```bash
cd /home/tesislinux/tesis/hdmi-overlay
buildimage
buildpetalinux
```

`buildpetalinux` reconstruye `petalinux-user-image` y empaqueta `BOOT.BIN`.
Los tres artefactos se tratan como un conjunto:

```text
images/linux/BOOT.BIN
images/linux/image.ub
images/linux/rootfs.ext4
```

`BOOT.BIN` contiene el arranque y bitstream; por eso el boot normal desde SD no
requiere Vivado ni JTAG.

## Grabación segura de la SD

Con el lector USB conectado a la VM, primero identificar la tarjeta:

```bash
lsblk -o NAME,MAJ:MIN,RM,SIZE,RO,TYPE,MOUNTPOINT
```

El helper `/home/tesislinux/bin/flashsd`:

- exige que el disco sea removible;
- rechaza el disco raíz de la VM;
- muestra el plan y exige escribir `FLASH /dev/...`;
- copia y compara hashes de `BOOT.BIN` e `image.ub`;
- escribe `rootfs.ext4`, ejecuta `e2fsck`, expande el filesystem y verifica la
  SD montándola en sólo lectura.

Si la tarjeta es `/dev/sdc`:

```bash
flashsd
```

Si tiene otro nombre:

```bash
SD_DISK=/dev/sdX flashsd
```

Nunca asumir el nombre del dispositivo sin revisar `lsblk`.

## Primer arranque después de grabar la SD

1. Apagar la placa e insertar la SD con el modo de boot en SD.
2. Conectar la placa a un puerto LAN del router.
3. Encenderla; `scripts/run.sh` localizará su IP por la MAC Ethernet.
4. Verificar `ip route`, `/etc/resolv.conf`, `date -u` y que `ntpd` esté activo.
5. Si se desea, verificar por consola serial la dirección con
   `ip -4 addr show eth0`.
6. Ejecutar el notebook `server/notebooks/nemotron_server.ipynb` hasta que
   `/health` y el túnel ngrok estén listos.
7. Instalar el ejecutable y servicio con `./scripts/run.sh`.
8. Reiniciar la placa y comprobar que inicia, sincroniza la hora y conecta a
   Colab sin ejecutar `server/run.sh` ni otro bridge en la PC.

La URL se guarda en `/etc/default/subtitle-overlay`; el valor predeterminado es:

```text
wss://passage-capacity-wistful.ngrok-free.dev/stt/stream
```

## Qué observar

Los estados esperados son: DHCP con ruta por defecto, hora válida, arranque del
servicio, transición WSS a `ready`, audio enviado y transcripts recibidos. El
firmware reintenta con backoff si Colab todavía no está disponible.

La prueba autónoma final consiste en reiniciar la placa sin ejecutar comandos
desde la PC y confirmar los subtítulos mientras el notebook de Colab permanece
activo.
