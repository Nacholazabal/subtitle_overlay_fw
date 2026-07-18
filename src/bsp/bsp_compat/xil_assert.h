#ifndef XIL_ASSERT_H_
#define XIL_ASSERT_H_

#define XIL_COMPONENT_IS_READY   0x11111111U
#define XIL_COMPONENT_IS_STARTED 0x22222222U

/*
 * Userspace keeps Xilinx assert call sites compile-compatible. SRC-M09: these
 * now match the genuine (non-fatal) Xilinx behavior of returning early from the
 * function when a precondition fails, instead of the previous no-op that let the
 * generated driver keep going and dereference an invalid pointer. Xilinx asserts
 * sit at function entry as precondition checks; the video_vtc wrappers validate
 * inputs before reaching them, so a well-formed call never trips these.
 */
#define Xil_AssertVoid(expr)           do { if (!(expr)) { return; } } while (0)
#define Xil_AssertNonvoid(expr)        do { if (!(expr)) { return 0; } } while (0)
#define Xil_AssertVoidAlways()         do { return; } while (0)
#define Xil_AssertNonvoidAlways()      do { return 0; } while (0)

#endif /* XIL_ASSERT_H_ */
