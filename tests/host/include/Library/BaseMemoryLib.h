#ifndef TEST_BASE_MEMORY_LIB_H_
#define TEST_BASE_MEMORY_LIB_H_

#include <string.h>

#define CopyMem(Destination, Source, Length) \
  memcpy((Destination), (Source), (Length))
#define ZeroMem(Buffer, Length) memset((Buffer), 0, (Length))
#define SetMem(Buffer, Length, Value) memset((Buffer), (Value), (Length))

#endif
