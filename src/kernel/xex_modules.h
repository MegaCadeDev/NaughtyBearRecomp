// Module lookups for XexGetModuleHandle / XexGetProcedureAddress.
#pragma once

#include <cstdint>
#include <string_view>

// Records the guest thunk address of every import of the loaded executable, by name.
void RegisterImportThunk(std::string_view name, uint32_t address);

// The guest address of the executable's own import thunk for this export, or 0.
uint32_t FindImportThunk(std::string_view name);

// Records a resource of the loaded executable (XEX_HEADER_RESOURCE_INFO entry).
void RegisterResource(std::string_view name, uint32_t address, uint32_t size);
