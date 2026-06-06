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

The application captures `16 kHz`, mono, `S16_LE` PCM from ALSA and streams
fixed 20 ms chunks over TCP. Runtime settings are read from environment
variables:

```sh
USB_AUDIO_PCM_DEVICE=plughw:1,0
USB_AUDIO_TCP_HOST=192.168.1.10
USB_AUDIO_TCP_PORT=5000
```

On the laptop, run the receiver before starting the board application:

```sh
scripts/audio_receiver.py --port 5000 --output usb_audio_capture.wav
```

Use `plughw` for early bring-up because ALSA can convert from the sound card's
native format to the STT-ready format. Move to `hw` only after the card is known
to support the format directly.

## Build Note

Real ALSA capture is enabled with:

```sh
make app USB_AUDIO_ENABLE_ALSA=1
```

The PetaLinux SDK/sysroot must provide `alsa/asoundlib.h` and `libasound`.
