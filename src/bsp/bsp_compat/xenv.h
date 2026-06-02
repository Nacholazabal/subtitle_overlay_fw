#ifndef XENV_H_
#define XENV_H_

/*
 * Older Xilinx BSP sources include xenv.h before using libc helpers such as
 * memset() and memcpy(). Keep that compatibility here instead of patching the
 * imported driver files.
 */
#include <string.h>

#endif /* XENV_H_ */
