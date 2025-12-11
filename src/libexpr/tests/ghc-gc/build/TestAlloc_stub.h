#include <HsFFI.h>
#if defined(__cplusplus)
extern "C" {
#endif
extern HsPtr test_alloc_bytes(HsWord64 a1);
extern void test_free_bytes(HsPtr a1);
extern HsPtr test_new_stable_ptr(HsPtr a1);
extern HsPtr test_deref_stable_ptr(HsPtr a1);
extern void test_free_stable_ptr(HsPtr a1);
extern void test_perform_gc(void);
extern HsInt32 test_get_magic(void);
#if defined(__cplusplus)
}
#endif

