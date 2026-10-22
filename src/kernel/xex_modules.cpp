// XexLoadImage, XexGetModuleHandle, XexGetProcedureAddress and XexGetModuleSection.
//
// The game asks for system modules (xam.xex, xboxkrnl.exe) at runtime and looks up
// exports by ordinal. Those exports resolve to the executable's own import thunks, which
// already call the runtime's implementation. Unleashed Recompiled leaves these as stubs.

#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xex_modules.h>
#include <os/logger.h>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_DLL_NOT_FOUND = 0xC0000135;
    constexpr uint32_t X_STATUS_ORDINAL_NOT_FOUND = 0xC0000138;

    // Module handles are opaque to the game; these just need to be distinct and non-zero.
    constexpr uint32_t MODULE_TITLE = 0x82000000;
    constexpr uint32_t MODULE_XAM = 0xFFFE0001;
    constexpr uint32_t MODULE_KERNEL = 0xFFFE0002;

    ankerl::unordered_dense::map<std::string, uint32_t> g_importThunks;

    struct Resource
    {
        uint32_t address;
        uint32_t size;
    };
    ankerl::unordered_dense::map<std::string, Resource> g_resources;

    uint32_t ModuleFromName(const char* name)
    {
        if (name == nullptr)
            return MODULE_TITLE;
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (lower == "xam.xex")
            return MODULE_XAM;
        if (lower == "xboxkrnl.exe")
            return MODULE_KERNEL;
        if (lower == "default.xex")
            return MODULE_TITLE;
        return 0;
    }
}

void RegisterImportThunk(std::string_view name, uint32_t address)
{
    g_importThunks.emplace(std::string(name), address);
}

void RegisterResource(std::string_view name, uint32_t address, uint32_t size)
{
    g_resources.emplace(std::string(name), Resource{ address, size });
}

uint32_t FindImportThunk(std::string_view name)
{
    auto it = g_importThunks.find(std::string(name));
    return it != g_importThunks.end() ? it->second : 0;
}

uint32_t XexGetModuleHandle(const char* moduleName, be<uint32_t>* handle)
{
    uint32_t module = ModuleFromName(moduleName);
    if (handle != nullptr)
        *handle = module;
    if (module == 0)
    {
        LOGF_WARNING("Unknown module \"{}\"", moduleName);
        return X_STATUS_DLL_NOT_FOUND;
    }
    return X_STATUS_SUCCESS;
}

uint32_t XexLoadImage(const char* moduleName, uint32_t flags, uint32_t minVersion, be<uint32_t>* handle)
{
    // Only the system modules exist; there is nothing else to load.
    uint32_t module = ModuleFromName(moduleName);
    if (handle != nullptr)
        *handle = module;
    if (module == 0 || module == MODULE_TITLE)
    {
        LOGF_WARNING("Cannot load module \"{}\"", moduleName);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return X_STATUS_SUCCESS;
}

uint32_t XexUnloadImage(uint32_t handle)
{
    return X_STATUS_SUCCESS;
}

uint32_t XexGetProcedureAddress(uint32_t handle, uint32_t ordinal, be<uint32_t>* address)
{
    if (address == nullptr)
        return X_STATUS_INVALID_PARAMETER;
    *address = 0;

    const std::unordered_map<size_t, const char*>* exports = nullptr;
    if (handle == MODULE_XAM)
        exports = &XamExports;
    else if (handle == MODULE_KERNEL)
        exports = &XboxKernelExports;

    const char* name = nullptr;
    if (exports != nullptr)
    {
        if (auto it = exports->find(ordinal); it != exports->end())
            name = it->second;
    }

    std::string_view exportName = name != nullptr ? std::string_view(name) : std::string_view();
    if (exportName.starts_with("__imp__"))
        exportName.remove_prefix(7);

    uint32_t thunk = exportName.empty() ? 0 : FindImportThunk(exportName);
    if (thunk == 0)
    {
        LOGF_WARNING("Export {} ({}) of module 0x{:08X} is not available", ordinal, exportName.empty() ? "unknown" : exportName, handle);
        return X_STATUS_ORDINAL_NOT_FOUND;
    }

    *address = thunk;
    return X_STATUS_SUCCESS;
}

uint32_t XexGetModuleSection(uint32_t handle, const char* sectionName, be<uint32_t>* data, be<uint32_t>* size)
{
    // Sections here are the title's resources (XEX_HEADER_RESOURCE_INFO), already in guest memory.
    auto it = sectionName != nullptr && handle == MODULE_TITLE ? g_resources.find(sectionName) : g_resources.end();
    if (it == g_resources.end())
    {
        LOGF_WARNING("Section \"{}\" of module 0x{:08X} not found", sectionName ? sectionName : "", handle);
        if (data != nullptr)
            *data = 0;
        if (size != nullptr)
            *size = 0;
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (data != nullptr)
        *data = it->second.address;
    if (size != nullptr)
        *size = it->second.size;
    return X_STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle);
GUEST_FUNCTION_HOOK(__imp__XexLoadImage, XexLoadImage);
GUEST_FUNCTION_HOOK(__imp__XexUnloadImage, XexUnloadImage);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress);
GUEST_FUNCTION_HOOK(__imp__XexGetModuleSection, XexGetModuleSection);
