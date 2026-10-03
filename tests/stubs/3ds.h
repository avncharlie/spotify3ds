#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
u64 osGetTime(void);
void *linearAlloc(size_t size);
void linearFree(void *ptr);
