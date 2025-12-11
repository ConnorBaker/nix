// Test for GHC GC allocation statistics
// Build: g++ -std=c++20 -I../../../include -I../../../../src/libutil/include test_alloc_stats.cc -o test_alloc_stats

#include <iostream>
#include <cassert>
#include <cstdlib>

// Minimal definitions to test the stats
extern "C" {

// From ghc-gc.cc stub implementations
void* nix_ghc_alloc_bytes(size_t size);
void* nix_ghc_alloc_bytes_atomic(size_t size);
size_t nix_ghc_get_alloc_count(void);
size_t nix_ghc_get_traced_alloc_count(void);
size_t nix_ghc_get_traced_alloc_bytes(void);
size_t nix_ghc_get_atomic_alloc_count(void);
size_t nix_ghc_get_atomic_alloc_bytes(void);
size_t nix_ghc_get_heap_size(void);
size_t nix_ghc_get_allocated_bytes(void);

}

int main() {
    std::cout << "=== GHC GC Allocation Statistics Test ===\n\n";

    // Get initial stats
    size_t initial_count = nix_ghc_get_alloc_count();
    size_t initial_bytes = nix_ghc_get_allocated_bytes();
    std::cout << "Initial state:\n";
    std::cout << "  Total allocations: " << initial_count << "\n";
    std::cout << "  Total bytes: " << initial_bytes << "\n\n";

    // Do some traced allocations (like Values)
    std::cout << "Performing 100 traced allocations (16 bytes each)...\n";
    for (int i = 0; i < 100; i++) {
        void* p = nix_ghc_alloc_bytes(16);
        assert(p != nullptr);
        // Note: In stub mode, memory is leaked (no GC)
    }

    size_t traced_count = nix_ghc_get_traced_alloc_count();
    size_t traced_bytes = nix_ghc_get_traced_alloc_bytes();
    std::cout << "After traced allocations:\n";
    std::cout << "  Traced alloc count: " << traced_count << "\n";
    std::cout << "  Traced alloc bytes: " << traced_bytes << "\n";
    assert(traced_count >= 100);
    assert(traced_bytes >= 1600);

    // Do some atomic allocations (like strings)
    std::cout << "\nPerforming 50 atomic allocations (64 bytes each)...\n";
    for (int i = 0; i < 50; i++) {
        void* p = nix_ghc_alloc_bytes_atomic(64);
        assert(p != nullptr);
    }

    size_t atomic_count = nix_ghc_get_atomic_alloc_count();
    size_t atomic_bytes = nix_ghc_get_atomic_alloc_bytes();
    std::cout << "After atomic allocations:\n";
    std::cout << "  Atomic alloc count: " << atomic_count << "\n";
    std::cout << "  Atomic alloc bytes: " << atomic_bytes << "\n";
    assert(atomic_count >= 50);
    assert(atomic_bytes >= 3200);

    // Check total stats
    size_t total_count = nix_ghc_get_alloc_count();
    size_t total_bytes = nix_ghc_get_allocated_bytes();
    size_t heap_size = nix_ghc_get_heap_size();
    std::cout << "\nFinal totals:\n";
    std::cout << "  Total allocations: " << total_count << "\n";
    std::cout << "  Total bytes allocated: " << total_bytes << "\n";
    std::cout << "  Heap size: " << heap_size << "\n";
    assert(total_count >= 150);
    assert(total_bytes >= 4800);
    assert(heap_size == total_bytes); // In stub mode, heap == allocated

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
