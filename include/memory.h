#pragma once
#include <windows.h>

// Memory manipulation functions
uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset);
void WriteGameMemory(uintptr_t address, const void* data, size_t size);
void SetPlayerPosition(uintptr_t base, uintptr_t baseOffset, double x, double y);
void UpdatePlayerValues(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2);
bool PatchMemory(uintptr_t address, const char* bytes, size_t length);
bool NopMemory(uintptr_t address, size_t length);