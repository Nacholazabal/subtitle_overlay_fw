# hdmi-vdma-client

Combined AXI VDMA dmaengine client for the HDMI demo passthrough path.

The module expects Linux `xilinx-vdma` to bind to the AXI VDMA node and this
client node in the device tree:

```dts
hdmi_vdma: hdmi-vdma {
    compatible = "hdmi-demo,vdma-client-v1";
    dmas = <&axi_vdma_0 0>, <&axi_vdma_0 1>;
    dma-names = "mm2s", "s2mm";
    memory-region = <&framebuf_reserved>;
};
```

It exposes `/dev/hdmi-vdma`, maps three shared RGB framebuffers to userspace,
and provides separate MM2S and S2MM ioctls through `hdmi_vdma.h`.

## Frame switching note

The current `*_SELECT` ioctls are a disruptive parking approximation: if a
channel is running, the driver terminates that dmaengine transfer and submits a
new repeated transfer for the selected frame. This is good enough for
passthrough bring-up, but it is not the same as AXI VDMA park mode changing the
read/write reference frame at a frame boundary. Expect a possible visual glitch
or partial frame during frame switches until real park/frame-boundary semantics
are added.

For PetaLinux, create or update the module recipe so `SRC_URI` includes:

```bitbake
file://Makefile
file://hdmi_vdma_client.c
file://hdmi_vdma.h
```
