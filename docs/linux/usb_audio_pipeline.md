# USB Audio Pipeline Bring-Up

## Kernel Ownership

USB0 is configured in host mode by `linux/dtsi/system-user.dtsi`. The kernel
enumerates the USB sound card and exposes capture through ALSA. The intended
kernel options are listed in `linux/kernel/usb_audio_host.cfg`.

Expected board checks:

```sh
lsusb
dmesg | grep -i snd-usb
cat /proc/asound/cards
arecord -l
```

## Userspace Ownership

The application captures native `48 kHz`, mono, `S16_LE` PCM from ALSA and streams
fixed 20 ms chunks over TCP. Runtime settings are read from environment
variables:

```sh
USB_AUDIO_PCM_DEVICE=hw:0,0
USB_AUDIO_TCP_HOST=192.168.1.20
USB_AUDIO_TCP_PORT=5000
```

`USB_AUDIO_TCP_HOST` must be the laptop or workstation IP reachable from the
board, not the board IP. On the laptop, run the receiver before starting the
board application:

```sh
scripts/audio_receiver.py --port 5000 --output usb_audio_capture.wav
```

For pipeline development before real STT integration, the same receiver can
emit fake STT-like subtitle events while still writing the WAV:

```sh
scripts/audio_receiver.py --port 5000 --output usb_audio_capture.wav \
  --simulate-stt --jsonl stt_events.jsonl
```

The simulator generates incremental `partial` transcript events every
`--chunk-word-interval` chunks and a `final` event every `--words-per-final`
generated words. These events are intended as a temporary stand-in for the
future flow from audio chunks to subtitle/bitmap updates.

When the firmware STT input active object is running on the board, the receiver
can also forward those fake transcript events back to the board over a separate
subtitle TCP channel:

```sh
scripts/audio_receiver.py --port 5000 --output usb_audio_capture.wav \
  --simulate-stt --send-subtitles --subtitle-host 192.168.1.10 --subtitle-port 5001
```

The board-side defaults are `SUBTITLE_STT_RX_HOST=0.0.0.0` and
`SUBTITLE_STT_RX_PORT=5001`.

The current bring-up path prefers `hw` over `plughw` so ALSA stays on the sound
card's native capture mode and avoids converter/plugin behavior during board
debugging.

## Working USB Topology

The currently validated USB audio path is:

- C-Media USB audio adapter, USB ID `0d8c:0014`.
- Adapter connected through a USB 2.0 hub.
- ALSA capture device `hw:0,0`.
- Native capture format `S16_LE`, mono, `48000 Hz`.
- Kernel option `CONFIG_USB_EHCI_TT_NEWSCHED=y`.

This matters because the adapter is a full-speed USB audio device and capture
uses isochronous transfers. On the Zynq USB host path, the USB 2.0 hub provides
the transaction translator between the high-speed host and the full-speed audio
device. The EHCI transaction-translator scheduler then decides how those
full-speed isochronous transfers are placed on the USB bus. Without the working
hub/scheduler combination, capture could enumerate but never deliver audio data.

The failure signature before this fix was:

- `arecord` created only a 44-byte WAV header.
- `dmesg` reported `cannot submit urb 0, error -28: not enough bandwidth`.
- The application sent the TCP stream header, then waited forever for the first
  ALSA chunk.

With `CONFIG_USB_EHCI_TT_NEWSCHED=y` active and the USB audio adapter behind
the USB 2.0 hub, native capture produces real data and the app streams chunk
sequence numbers with `dropped=0`.

Useful board checks:

```sh
zcat /proc/config.gz | grep CONFIG_USB_EHCI_TT_NEWSCHED
lsusb
cat /proc/asound/cards
arecord -D hw:0,0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/hub-native.wav
ls -lh /tmp/hub-native.wav
```

The `arecord` output should be much larger than 44 bytes. A 5 second mono
48 kHz S16_LE file should be roughly 469 KiB.

## Build Note

The VM build and run scripts enable ALSA capture by default. A direct `build`
or `run` therefore builds the real audio path. Manual make-based builds inside
the VM do the same:

```sh
make app
```

The PetaLinux SDK/sysroot must provide `alsa/asoundlib.h` and `libasound`.

For kernel or rootfs changes, update the SD card as a matched set:

- `images/linux/BOOT.BIN` to the FAT boot partition.
- `images/linux/image.ub` to the FAT boot partition.
- `images/linux/rootfs.ext4` to the ext4 rootfs partition.

Do not mix a freshly built rootfs with a stale `image.ub`. That can leave kernel
modules built for a different kernel image and produce `module_layout` errors
on the board.
