#ifndef XIL_IO_H_
#define XIL_IO_H_

#include "xil_types.h"

/*
 * Linux userspace passes mmap()'d virtual AXI-Lite bases to BSP
 * CfgInitialize() calls. These accessors therefore dereference virtual
 * addresses, never raw physical PL addresses.
 */
#define Xil_Out32(Addr, Value) \
	(*((volatile u32 *)(UINTPTR)(Addr)) = (u32)(Value))
#define Xil_In32(Addr) \
	(*((volatile u32 *)(UINTPTR)(Addr)))

#define Xil_Out16(Addr, Value) \
	(*((volatile u16 *)(UINTPTR)(Addr)) = (u16)(Value))
#define Xil_In16(Addr) \
	(*((volatile u16 *)(UINTPTR)(Addr)))

#define Xil_Out8(Addr, Value) \
	(*((volatile u8 *)(UINTPTR)(Addr)) = (u8)(Value))
#define Xil_In8(Addr) \
	(*((volatile u8 *)(UINTPTR)(Addr)))

#endif /* XIL_IO_H_ */
