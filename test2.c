#include <stdio.h>
#include "malloc.h"


int main() {
    
    int* ptr = (int*) malloc(sizeof(int));
printf("To free %p\n",ptr);
    free(ptr);

    ptr = (int*) malloc(sizeof(int) * 100);
printf("To free %p\n",ptr);
    free(ptr);

    return 0;
}
