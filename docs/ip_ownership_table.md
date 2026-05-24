| IP / Block | VLNV | Role in design | Recommended Linux owner | Why |
|---|---|---|---|---|
| `processing_system7_0` | `xilinx.com:ip:processing_system7:5.5` | Zynq PS, DDR, GP0, HP0, IRQ_F2P, clocks/resets | Linux kernel | Kernel already owns PS resources, DDR, IRQ routing, and PS peripheral infrastructure. |
| `axi_vdma_0` | `xilinx.com:ip:axi_vdma:6.3` | Frame DMA between AXI4-Stream video and DDR | Linux driver or kernel-assisted path | This is the hardest block to fake safely in userspace because of DMA buffers, cache coherency, and interrupts. |
| `v_vid_in_axi4s_0` | `xilinx.com:ip:v_vid_in_axi4s:4.0` | Converts input video bus to AXI4-Stream | Userspace MMIO or leave hardware-static | Simple video bridge with a control register interface; low ownership complexity compared to VDMA. |
| `v_axi4s_vid_out_0` | `xilinx.com:ip:v_axi4s_vid_out:4.0` | Converts AXI4-Stream back to video output bus | Userspace MMIO or leave hardware-static | Similar to `v_vid_in_axi4s_0`; mainly format/timing control. |
| `v_tc_0` | `xilinx.com:ip:v_tc:6.1` | Output timing controller for HDMI out | Userspace MMIO | AXI-Lite controlled and timing-oriented. Usually manageable from userspace if clocks and resets are already stable. |
| `v_tc_1` | `xilinx.com:ip:v_tc:6.1` | Input timing detection for HDMI in | Userspace MMIO | Good candidate for simple register access; likely used for timing status and mode detection. |
| `axi_gpio_video` | `xilinx.com:ip:axi_gpio:2.0` | Video-side GPIO and input status, with interrupt | Prefer Linux GPIO if exposed cleanly, otherwise userspace MMIO | It is simple enough for MMIO, but Linux GPIO style is cleaner if the signal is surfaced that way. |
| `axi_dynclk_0` | `digilentinc.com:ip:axi_dynclk:1.0` | Dynamic pixel clock programming | Userspace MMIO | Custom AXI-Lite control block, good fit for direct mapped register writes from userspace. |
| `dvi2rgb_0` | `digilentinc.com:ip:dvi2rgb:1.7` | TMDS decode and HDMI/DDC input frontend | Mixed: hardware-static plus Linux I2C for DDC if needed | The TMDS datapath is in PL. The DDC interface should lean Linux/I2C if software needs to own EDID or link-side I2C behavior. |
| `rgb2dvi_0` | `digilentinc.com:ip:rgb2dvi:1.3` | TMDS encode for HDMI output | Usually hardware-static | No obvious Linux-owned resource beyond clocks/resets. Likely configured once and left alone. |
| `axi_bram_ctrl_0` | `xilinx.com:ip:axi_bram_ctrl:4.1` | PS AXI access to subtitle BRAM | Userspace MMIO via mapped AXI address | This is a strong userspace candidate because it is just memory-mapped storage behind AXI. |
| `subtitle_mask_mem_0` | `IOELECTRONICS:user:subtitle_mask_mem:1.0` | Dual-port BRAM storing subtitle mask data | Access through `axi_bram_ctrl_0` | Software should not own the raw BRAM primitive directly; it should write through the AXI BRAM controller window. |
| `axis_video_overlay_r_0` | `IOELECTRONICS:user:axis_video_overlay_rect:6.2` | Custom overlay block mixing video stream with subtitle mask | Userspace MMIO | This is your own control IP, and it is exactly the kind of block that benefits from a small userspace register driver. |
| `xlconcat_0` | `xilinx.com:ip:xlconcat:2.1` | Aggregates multiple PL interrupts to PS IRQ_F2P | Kernel-owned wiring | This is not a software-controlled runtime IP in practice. |
| `axi_mem_intercon` | `xilinx.com:ip:axi_interconnect:2.1` | AXI fabric between VDMA and PS HP0 | Kernel / transparent hardware | Not an application software target. |
| `ps7_0_axi_periph` | `xilinx.com:ip:axi_interconnect:2.1` | AXI fabric from PS GP0 to control registers | Kernel / transparent hardware | Not a driver target; just the control path fabric. |
| `proc_sys_reset_0` | `xilinx.com:ip:proc_sys_reset:5.0` | Reset sequencing for input path | Hardware-static | Runtime software should usually not own this directly. |
| `rst_processing_system7_0_100M` | `xilinx.com:ip:proc_sys_reset:5.0` | Reset sequencing for AXI-Lite domain | Hardware-static | Same story; software depends on the result but should not manage it like a device. |
| `rst_processing_system7_0_150M` | `xilinx.com:ip:proc_sys_reset:5.0` | Reset sequencing for video AXI domain | Hardware-static | Same story. |
| `proc_sys_reset_pxl` | `xilinx.com:ip:proc_sys_reset:5.0` | Pixel-domain reset handling | Hardware-static | Same story. |
| `proc_sys_reset_video_axis` | `xilinx.com:ip:proc_sys_reset:5.0` | AXIS-domain reset handling | Hardware-static | Same story. |
| `ila_0` | `xilinx.com:ip:ila:6.2` | Integrated logic analyzer for debug | Vivado debug only | Not part of Linux runtime ownership. |
