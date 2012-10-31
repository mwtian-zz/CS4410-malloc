#include <stdio.h>
#include "malloc.h"

#define NUM_MALLOCS 5000

int main() {
    int i;
    int* ptrs[NUM_MALLOCS];

    for(i = 0; i < NUM_MALLOCS; i++){
        ptrs[i] = (int*) calloc((i+1), sizeof(int));
        ptrs[i][i] = i;
    }

    for(i = 0; i < NUM_MALLOCS; i++){
        ptrs[i] = (int*) realloc(ptrs[i], (i+5) * sizeof(int));
        ptrs[i][i] = i;
    }

    for(i = 0; i < NUM_MALLOCS; i++) {
        free(ptrs[i]);
    }

    return 0;
}
