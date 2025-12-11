// Test program for GHC RTS integration
// Verifies that we can initialize the GHC RTS and call Haskell FFI exports.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

// GHC RTS headers
extern "C" {
#include "HsFFI.h"
#include "Rts.h"
#include "RtsAPI.h"
}

// Forward declarations for Haskell-exported functions (from TestAlloc.hs)
extern "C" {
    void* test_alloc_bytes(size_t size);
    void test_free_bytes(void* ptr);
    void* test_new_stable_ptr(void* ptr);
    void* test_deref_stable_ptr(void* stable);
    void test_free_stable_ptr(void* stable);
    void test_perform_gc();
    int test_get_magic();
}

// Test helpers
#define TEST(name) \
    printf("Testing %s... ", #name); \
    fflush(stdout);

#define PASS() printf("PASS\n")
#define FAIL(msg) do { printf("FAIL: %s\n", msg); failures++; } while(0)

int main(int argc, char* argv[])
{
    int failures = 0;

    printf("=== GHC RTS Integration Test ===\n\n");

    // Initialize GHC RTS
    printf("Initializing GHC RTS...\n");
    RtsConfig conf = defaultRtsConfig;
    conf.rts_opts_enabled = RtsOptsAll;
    hs_init_ghc(&argc, &argv, conf);
    printf("GHC RTS initialized successfully.\n\n");

    // Test 1: Magic value (basic FFI)
    TEST(get_magic);
    {
        int magic = test_get_magic();
        if (magic == 42) {
            PASS();
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "expected 42, got %d", magic);
            FAIL(buf);
        }
    }

    // Test 2: Basic allocation
    TEST(alloc_bytes);
    {
        void* ptr = test_alloc_bytes(1024);
        if (ptr != nullptr) {
            // Write to the memory to verify it's usable
            memset(ptr, 0xAB, 1024);
            test_free_bytes(ptr);
            PASS();
        } else {
            FAIL("allocation returned nullptr");
        }
    }

    // Test 3: Zero-size allocation
    TEST(alloc_zero_bytes);
    {
        void* ptr = test_alloc_bytes(0);
        if (ptr == nullptr) {
            PASS();
        } else {
            FAIL("expected nullptr for zero-size allocation");
        }
    }

    // Test 4: StablePtr creation and dereferencing
    TEST(stable_ptr_roundtrip);
    {
        // Allocate some memory
        void* original = test_alloc_bytes(64);
        if (original == nullptr) {
            FAIL("failed to allocate memory");
        } else {
            // Create a StablePtr
            void* stable = test_new_stable_ptr(original);
            if (stable == nullptr) {
                FAIL("failed to create StablePtr");
            } else {
                // Dereference it
                void* dereffed = test_deref_stable_ptr(stable);
                if (dereffed == original) {
                    // Free the StablePtr
                    test_free_stable_ptr(stable);
                    PASS();
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "expected %p, got %p", original, dereffed);
                    FAIL(buf);
                }
            }
            test_free_bytes(original);
        }
    }

    // Test 5: StablePtr survives GC
    TEST(stable_ptr_survives_gc);
    {
        void* original = test_alloc_bytes(64);
        if (original == nullptr) {
            FAIL("failed to allocate memory");
        } else {
            void* stable = test_new_stable_ptr(original);
            if (stable == nullptr) {
                FAIL("failed to create StablePtr");
            } else {
                // Trigger GC
                test_perform_gc();

                // Dereference after GC
                void* dereffed = test_deref_stable_ptr(stable);
                if (dereffed == original) {
                    test_free_stable_ptr(stable);
                    PASS();
                } else {
                    FAIL("StablePtr value changed after GC");
                }
            }
            test_free_bytes(original);
        }
    }

    // Test 6: Null StablePtr handling
    TEST(null_stable_ptr);
    {
        void* dereffed = test_deref_stable_ptr(nullptr);
        if (dereffed == nullptr) {
            test_free_stable_ptr(nullptr); // Should not crash
            PASS();
        } else {
            FAIL("expected nullptr from deref of null StablePtr");
        }
    }

    // Test 7: Multiple allocations
    TEST(multiple_allocations);
    {
        const int count = 100;
        void* ptrs[count];
        bool success = true;

        for (int i = 0; i < count && success; i++) {
            ptrs[i] = test_alloc_bytes(256);
            if (ptrs[i] == nullptr) {
                success = false;
            }
        }

        if (success) {
            // Trigger GC while memory is allocated
            test_perform_gc();

            for (int i = 0; i < count; i++) {
                test_free_bytes(ptrs[i]);
            }
            PASS();
        } else {
            FAIL("allocation failed during multiple allocations");
        }
    }

    printf("\n");

    // Shutdown GHC RTS
    printf("Shutting down GHC RTS...\n");
    hs_exit();
    printf("GHC RTS shutdown complete.\n\n");

    // Summary
    if (failures == 0) {
        printf("=== All tests passed! ===\n");
        return 0;
    } else {
        printf("=== %d test(s) failed ===\n", failures);
        return 1;
    }
}
