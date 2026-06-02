#ifndef XIL_ASSERT_H_
#define XIL_ASSERT_H_

#define XIL_COMPONENT_IS_READY   0x11111111U
#define XIL_COMPONENT_IS_STARTED 0x22222222U

/*
 * Userspace keeps Xilinx assert call sites compile-compatible. Production
 * behavior is non-fatal so polling diagnostics can report the real failure.
 */
#define Xil_AssertVoid(expr)           do { (void)(expr); } while (0)
#define Xil_AssertNonvoid(expr)        do { (void)(expr); } while (0)
#define Xil_AssertVoidAlways()         do { } while (0)
#define Xil_AssertNonvoidAlways(ret)   return (ret)

#endif /* XIL_ASSERT_H_ */
