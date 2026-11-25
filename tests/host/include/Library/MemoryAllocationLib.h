#ifndef TEST_MEMORY_ALLOCATION_LIB_H_
#define TEST_MEMORY_ALLOCATION_LIB_H_

#include <stdlib.h>

#define AllocatePool(Size) malloc(Size)
#define AllocateZeroPool(Size) calloc(1, (Size))
#define FreePool(Buffer) free(Buffer)

#endif
