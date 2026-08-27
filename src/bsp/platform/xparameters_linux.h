#ifndef XPARAMETERS_LINUX_H_
#define XPARAMETERS_LINUX_H_

/*
 * Linux userspace subset of the SDK-generated hardware parameters.
 *
 * Review these values whenever the Vivado address assignment or the
 * SDK-generated xparameters.h changes. The addresses are physical AXI-Lite
 * addresses; Linux userspace must map them before register access.
 *
 * Hand-maintained against the flashed bitstream, so it carries only the
 * definitions the code actually references — a second name for one address is
 * how a wrong-address bug gets written.
 */

#define XPAR_AXI_DYNCLK_0_BASEADDR 0x43C00000U

#define XPAR_XVTC_NUM_INSTANCES 2U

// V_TC_0 drives the output generator; V_TC_1 detects the input timing.
#define XPAR_V_TC_0_DEVICE_ID 0U
#define XPAR_V_TC_0_BASEADDR  0x43C10000U

#define XPAR_V_TC_1_DEVICE_ID 1U
#define XPAR_V_TC_1_BASEADDR  0x43C20000U

#define XPAR_AXIS_VIDEO_OVERLAY_R_0_BASEADDR 0x43C30000U

#define XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR 0x40000000U
#define XPAR_AXI_BRAM_CTRL_0_S_AXI_HIGHADDR 0x40007FFFU

#define XPAR_AXI_GPIO_VIDEO_BASEADDR 0x41200000U

#endif /* XPARAMETERS_LINUX_H_ */
