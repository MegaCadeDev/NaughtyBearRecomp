// Saves and storage devices (XamContent*, device selector, enumeration) and profile
// settings, for Naughty Bear.
//
// Unleashed Recompiled registers Sonic's one save container up front and keeps the
// registry in memory; Naughty Bear picks a device, creates containers and enumerates them,
// so this keeps containers on disk instead: one hard drive (device 1), and each save
// container is a directory under the save folder with its XCONTENT_DATA stored alongside,
// so saves persist between runs.
//
// Requests made with an XOVERLAPPED complete immediately the way Xenia does it: the result
// goes into the overlapped, its event is signalled, and the call returns ERROR_IO_PENDING.

#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <os/logger.h>
#include <user/paths.h>

void SignalEventHandle(uint32_t handle);

// Unleashed Recompiled's host-side content functions (kernel/xam.cpp), still used for the
// game's own read-only roots ("game:", "D:").
uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped);
uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped);

namespace
{
    constexpr uint32_t HDD_DEVICE_ID = 1;
    constexpr uint32_t ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t ERROR_FILE_NOT_FOUND = 0x2;
    constexpr uint32_t ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t XN_SYS_UI = 0x00000009;
    constexpr const char* CONTENT_METADATA = "__content.bin";

    // Completes a request: through the overlapped when given, otherwise the plain result.
    uint32_t FinishImpl(const char* caller, XXOVERLAPPED* overlapped, uint32_t result, uint32_t length = 0)
    {
        if (overlapped == nullptr)
            return result;
        overlapped->Error = result;
        overlapped->Length = length;
        overlapped->dwExtendedError = 0;
        if (overlapped->pCompletionRoutine != 0)
            LOG_UTILITY("XOVERLAPPED completion routines are not supported");
        SignalEventHandle(overlapped->hEvent);
        return ERROR_IO_PENDING;
    }

#define Finish(...) FinishImpl(__func__, __VA_ARGS__)

    std::filesystem::path ContentPath(const XCONTENT_DATA& data)
    {
        std::string name(data.szFileName, strnlen(data.szFileName, XCONTENT_MAX_FILENAME));
        return GetSavePath(false) / name;
    }

    struct ContentEnumerator final : XamEnumeratorBase
    {
        std::vector<XCONTENT_DATA> items;
        size_t position = 0;
        uint32_t fetch = 1;

        uint32_t Next(void* buffer) override
        {
            if (position >= items.size())
                return -1;
            uint32_t count = 0;
            for (; count < fetch && position < items.size(); count++, position++)
            {
                if (buffer != nullptr)
                    memcpy((uint8_t*)buffer + count * sizeof(XCONTENT_DATA), &items[position], sizeof(XCONTENT_DATA));
            }
            return count;
        }
    };
}

uint32_t NbXamShowDeviceSelectorUI(uint32_t userIndex, uint32_t contentType, uint32_t contentFlags, uint64_t totalRequested,
    be<uint32_t>* deviceId, XXOVERLAPPED* overlapped)
{
    // The system UI opens and closes at once, having picked the hard drive.
    XamNotifyEnqueueEvent(XN_SYS_UI, 1);
    if (deviceId != nullptr)
        *deviceId = HDD_DEVICE_ID;
    XamNotifyEnqueueEvent(XN_SYS_UI, 0);
    return Finish(overlapped, ERROR_SUCCESS);
}

uint32_t NbXamContentGetDeviceState(uint32_t deviceId, XXOVERLAPPED* overlapped)
{
    return Finish(overlapped, deviceId == HDD_DEVICE_ID ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED);
}

uint32_t NbXamContentGetDeviceData(uint32_t deviceId, XDEVICE_DATA* data)
{
    if (deviceId != HDD_DEVICE_ID)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(data, 0, sizeof(*data));
    data->DeviceID = deviceId;
    data->DeviceType = XCONTENTDEVICETYPE_HDD;
    data->ulDeviceBytes = 20ull << 30;
    data->ulDeviceFreeBytes = 16ull << 30;
    const char* name = "Hard Drive";
    for (size_t i = 0; name[i] != 0; i++)
        data->wszName[i] = uint16_t(name[i]);
    return ERROR_SUCCESS;
}

uint32_t NbXamContentCreateEx(uint32_t userIndex, const char* rootName, const XCONTENT_DATA* contentData, uint32_t contentFlags,
    be<uint32_t>* disposition, be<uint32_t>* licenseMask, uint32_t fileCacheSize, uint64_t contentSize, XXOVERLAPPED* overlapped)
{
    if (contentData == nullptr || rootName == nullptr)
        return Finish(overlapped, ERROR_BAD_ARGUMENTS);

    if (contentData->dwContentType != XCONTENTTYPE_SAVEDATA)
        return Finish(overlapped, XamContentCreateEx(userIndex, rootName, contentData, contentFlags, disposition, licenseMask, fileCacheSize, contentSize, nullptr));

    std::filesystem::path path = ContentPath(*contentData);
    std::error_code ec;
    bool exists = std::filesystem::is_directory(path, ec);
    uint32_t mode = contentFlags & 0xF;

    if (!exists && (mode == OPEN_EXISTING || mode == 5 /* TRUNCATE_EXISTING */))
        return Finish(overlapped, ERROR_PATH_NOT_FOUND);
    if (exists && mode == CREATE_NEW)
        return Finish(overlapped, ERROR_ALREADY_EXISTS);

    if (!exists)
    {
        std::filesystem::create_directories(path, ec);
        if (ec)
            return Finish(overlapped, ERROR_PATH_NOT_FOUND);
        XCONTENT_DATA metadata = *contentData;
        metadata.DeviceID = HDD_DEVICE_ID;
        std::ofstream(path / CONTENT_METADATA, std::ios::binary).write((const char*)&metadata, sizeof(metadata));
    }

    if (disposition != nullptr)
        *disposition = exists ? XCONTENT_EXISTING : XCONTENT_NEW;
    if (licenseMask != nullptr)
        *licenseMask = 0;
    XamRootCreate(rootName, (const char*)path.u8string().c_str());
    LOGFN("Save: \"{}\" mounted as {}: ({})", contentData->szFileName, rootName, exists ? "existing" : "new");
    return Finish(overlapped, ERROR_SUCCESS);
}

uint32_t NbXamContentClose(const char* rootName, XXOVERLAPPED* overlapped)
{
    XamContentClose(rootName, nullptr);
    return Finish(overlapped, ERROR_SUCCESS);
}

uint32_t NbXamContentDelete(uint32_t userIndex, const XCONTENT_DATA* contentData, XXOVERLAPPED* overlapped)
{
    if (contentData == nullptr || contentData->dwContentType != XCONTENTTYPE_SAVEDATA)
        return Finish(overlapped, ERROR_FILE_NOT_FOUND);
    std::error_code ec;
    bool removed = std::filesystem::remove_all(ContentPath(*contentData), ec) > 0;
    return Finish(overlapped, removed ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
}

uint32_t NbXamContentGetCreator(uint32_t userIndex, const XCONTENT_DATA* contentData, be<uint32_t>* isCreator, be<uint64_t>* xuid, XXOVERLAPPED* overlapped)
{
    if (isCreator != nullptr)
        *isCreator = 1;
    if (xuid != nullptr)
        *xuid = 0xB13EBABEBABEBABEull;
    return Finish(overlapped, ERROR_SUCCESS);
}

uint32_t NbXamContentCreateEnumerator(uint32_t userIndex, uint32_t deviceId, uint32_t contentType, uint32_t contentFlags,
    uint32_t itemCount, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    auto* enumerator = CreateKernelObject<ContentEnumerator>();
    enumerator->fetch = std::max<uint32_t>(itemCount, 1);

    if (contentType == XCONTENTTYPE_SAVEDATA && (deviceId == 0 || deviceId == HDD_DEVICE_ID))
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(GetSavePath(false), ec))
        {
            XCONTENT_DATA data{};
            std::ifstream file(entry.path() / CONTENT_METADATA, std::ios::binary);
            if (file.read((char*)&data, sizeof(data)))
                enumerator->items.push_back(data);
        }
    }

    if (bufferSize != nullptr)
        *bufferSize = sizeof(XCONTENT_DATA) * enumerator->fetch;
    *handle = GetKernelHandle(enumerator);
    return ERROR_SUCCESS;
}

uint32_t NbXamEnumerate(uint32_t handle, uint32_t flags, void* buffer, uint32_t bufferSize, be<uint32_t>* itemsReturned, XXOVERLAPPED* overlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(handle);
    uint32_t count = enumerator->Next(buffer);
    if (count == uint32_t(-1))
    {
        if (itemsReturned != nullptr)
            *itemsReturned = 0;
        return Finish(overlapped, ERROR_NO_MORE_FILES);
    }
    if (itemsReturned != nullptr)
        *itemsReturned = count;
    return Finish(overlapped, ERROR_SUCCESS, count);
}

// XamUserReadProfileSettings: returns every requested setting as "no value", so the game
// uses its defaults. Result: { count, pSettings } followed by XUSER_PROFILE_SETTING[count]
// (0x28 bytes each: source, user, setting id, data).
uint32_t NbXamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, void* xuids,
    uint32_t settingCount, be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer, XXOVERLAPPED* overlapped)
{
    constexpr uint32_t SETTING_SIZE = 0x28;
    uint32_t required = 8 + settingCount * SETTING_SIZE;
    if (bufferSize == nullptr)
        return Finish(overlapped, ERROR_BAD_ARGUMENTS);
    if (buffer == nullptr || *bufferSize < required)
    {
        *bufferSize = required;
        return Finish(overlapped, ERROR_INSUFFICIENT_BUFFER);
    }

    memset(buffer, 0, required);
    *(be<uint32_t>*)buffer = settingCount;
    *(be<uint32_t>*)(buffer + 4) = g_memory.MapVirtual(buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        uint8_t* setting = buffer + 8 + i * SETTING_SIZE;
        *(be<uint32_t>*)(setting + 8) = userIndex;              // user
        *(be<uint32_t>*)(setting + 16) = uint32_t(settingIds[i]); // setting id
        setting[24] = uint8_t((uint32_t(settingIds[i]) >> 28) & 0xF); // data type
    }
    return Finish(overlapped, ERROR_SUCCESS);
}

GUEST_FUNCTION_HOOK(__imp__XamShowDeviceSelectorUI, NbXamShowDeviceSelectorUI);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceState, NbXamContentGetDeviceState);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceData, NbXamContentGetDeviceData);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEx, NbXamContentCreateEx);
GUEST_FUNCTION_HOOK(__imp__XamContentClose, NbXamContentClose);
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, NbXamContentDelete);
GUEST_FUNCTION_HOOK(__imp__XamContentGetCreator, NbXamContentGetCreator);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEnumerator, NbXamContentCreateEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamEnumerate, NbXamEnumerate);
GUEST_FUNCTION_HOOK(__imp__XamUserReadProfileSettings, NbXamUserReadProfileSettings);
