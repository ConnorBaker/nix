#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main() {
    uint64_t* ptr = (uint64_t*)malloc(16);  // Not calloc!
    printf("p0 = 0x%lx\n", ptr[0]);
    printf("p1 = 0x%lx\n", ptr[1]);
    printf("discriminator = 0x%lx\n", ptr[0] & 0x7);
    
    // Test if pdThunk (value 1) appears
    if ((ptr[0] & 0x7) == 1) {
        printf("FOUND pdThunk discriminator!\n");
    }
    
    free(ptr);
    return 0;
}
