// XAM user, enumeration, task and miscellaneous functions Naughty Bear imports that
// Unleashed Recompiled does not, plus a few kernel ones (mutants, SLists, DPCs).
//
// The console is offline with one signed-in local profile ("Player") on user index 0.

#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <os/logger.h>

namespace
{
    constexpr uint64_t PLAYER_XUID = 0xB13EBABEBABEBABEull; // as in Unleashed Recompiled
    constexpr uint32_t TITLE_ID = 0x464F07D8;
    constexpr uint32_t ERROR_NOT_FOUND = 0x490;
    constexpr uint32_t ERROR_FUNCTION_FAILED = 0x65B;
    constexpr uint32_t ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t X_STATUS_SUCCESS = 0;
    constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
    constexpr uint32_t XACHIEVEMENT_DETAILS_SIZE = 0x1C;

    // An enumerator with nothing in it (achievements, stats and other online data).
    uint32_t CreateEmptyEnumerator(uint32_t itemSize, uint32_t itemCount, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (handle == nullptr)
            return ERROR_INVALID_PARAMETER;
        if (bufferSize != nullptr)
            *bufferSize = itemSize * std::max<uint32_t>(itemCount, 1);
        *handle = GetKernelHandle(CreateKernelObject<XamEnumeratorBase>());
        return ERROR_SUCCESS;
    }
}

uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t size)
{
    if (userIndex != 0)
        return ERROR_NO_SUCH_USER;
    if (buffer != nullptr && size != 0)
        strncpy(buffer, "Player", size - 1)[size - 1] = 0;
    return ERROR_SUCCESS;
}

uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t typeMask, be<uint64_t>* xuid)
{
    if (userIndex != 0)
        return ERROR_NO_SUCH_USER;
    if (xuid != nullptr)
        *xuid = PLAYER_XUID;
    return ERROR_SUCCESS;
}

uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t privilege, be<uint32_t>* result)
{
    if (userIndex != 0)
        return ERROR_NO_SUCH_USER;
    if (result != nullptr)
        *result = 1;
    return ERROR_SUCCESS;
}

uint32_t XamUserAreUsersFriends(uint32_t userIndex, void* xuids, uint32_t count, be<uint32_t>* result, void* overlapped)
{
    if (result != nullptr)
        *result = 0;
    return ERROR_SUCCESS;
}

uint32_t XamUserGetMembershipTierFromXUID(uint64_t xuid)
{
    return 3; // Silver: no Xbox Live Gold
}

uint32_t XamUserGetOnlineCountryFromXUID(uint64_t xuid)
{
    return 103; // United States
}

uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t detailFlags,
    uint32_t startingIndex, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    return CreateEmptyEnumerator(XACHIEVEMENT_DETAILS_SIZE, count, bufferSize, handle);
}

uint32_t XamUserCreateStatsEnumerator(uint32_t titleId, uint32_t rankStart, uint32_t xuidPivot, uint32_t numRows,
    uint32_t numStatsSpecs, uint32_t statsSpecs, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    return CreateEmptyEnumerator(0x20, numRows, bufferSize, handle);
}

uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
    uint32_t extraSize, uint32_t itemCount, uint32_t flags, be<uint32_t>* handle)
{
    return CreateEmptyEnumerator(1, itemCount, nullptr, handle);
}

uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* structure)
{
    if (structure != nullptr)
        *structure = 0;
    return ERROR_NOT_FOUND;
}

// Dashboard UI (friends, marketplace, gamer cards): nothing to show offline.
uint32_t XamShowUIStub() { return ERROR_SUCCESS; }

uint32_t XamLoaderGetLaunchData(void* buffer, uint32_t size)
{
    return ERROR_NOT_FOUND;
}

uint32_t XamLoaderSetLaunchData(void* buffer, uint32_t size)
{
    return ERROR_SUCCESS;
}

// XamTaskSchedule(callback, message, optional, handle): runs callback(message) on a thread.
uint32_t XamTaskSchedule(uint32_t callback, uint32_t message, uint32_t optional, be<uint32_t>* handle)
{
    uint32_t threadId = 0;
    auto* thread = GuestThread::Start({ callback, message, 0 }, &threadId);
    if (handle != nullptr)
        *handle = GetKernelHandle(thread);
    return X_STATUS_SUCCESS;
}

uint32_t XamTaskShouldExit(uint32_t task)
{
    return 0;
}

uint32_t XamTaskCloseHandle(uint32_t handle)
{
    return X_STATUS_SUCCESS; // the thread object stays alive until it finishes
}

// Online sessions and voice: unavailable offline.
uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
{
    if (handle != nullptr)
        *handle = 0;
    return ERROR_FUNCTION_FAILED;
}

uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
{
    if (object != nullptr)
        *object = 0;
    return ERROR_FUNCTION_FAILED;
}

uint32_t XamVoiceCreate(uint32_t userIndex, uint32_t flags, be<uint32_t>* voice)
{
    if (voice != nullptr)
        *voice = 0;
    return ERROR_FUNCTION_FAILED;
}

uint32_t XamVoiceClose(uint32_t voice) { return ERROR_SUCCESS; }
uint32_t XamVoiceHeadsetPresent(uint32_t voice) { return 0; }
uint32_t XamVoiceSubmitPacket(uint32_t voice) { return ERROR_FUNCTION_FAILED; }

uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* result)
{
    void* memory = g_userHeap.Alloc(std::max<uint32_t>(size, 1));
    if (memory == nullptr)
        return 0x8007000E; // E_OUTOFMEMORY
    memset(memory, 0, size);
    *result = g_memory.MapVirtual(memory);
    return 0;
}

uint32_t XamFree(uint32_t pointer)
{
    if (pointer != 0)
        g_userHeap.Free(g_memory.Translate(pointer));
    return 0;
}

uint32_t XamContentFlush(const char* root, void* overlapped) { return ERROR_SUCCESS; }
uint32_t XNotifyPositionUI(uint32_t position) { return 0; }
uint32_t XMsgCancelIORequest(uint32_t overlapped, uint32_t wait) { return 0; }
uint32_t XNetLogonGetTitleID() { return TITLE_ID; }

uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
{
    if (config != nullptr)
        *config = 0x00010001; // stereo, as Xenia reports
    return 0;
}

// Console keys: there is no console to sign with or verify against.
uint32_t XeKeysConsolePrivateKeySign(void* hash, void* signature) { return 0; }
uint32_t XeKeysConsoleSignatureVerification(void* hash, void* signature, be<uint32_t>* result)
{
    if (result != nullptr)
        *result = 0;
    return 0;
}

// ---- Kernel ----

// Mutants (kernel mutexes), owned recursively by a thread.
struct Mutant final : KernelObject
{
    std::recursive_timed_mutex mutex;

    uint32_t Wait(uint32_t timeout) override
    {
        if (timeout == INFINITE)
        {
            mutex.lock();
            return STATUS_WAIT_0;
        }
        return mutex.try_lock_for(std::chrono::milliseconds(timeout)) ? STATUS_WAIT_0 : STATUS_TIMEOUT;
    }
};

uint32_t NtCreateMutant(be<uint32_t>* handle, void* attributes, uint32_t initialOwner)
{
    auto* mutant = CreateKernelObject<Mutant>();
    if (initialOwner)
        mutant->mutex.lock();
    *handle = GetKernelHandle(mutant);
    return X_STATUS_SUCCESS;
}

uint32_t NtReleaseMutant(uint32_t handle, be<uint32_t>* previousCount)
{
    if (!IsKernelObject(handle))
        return X_STATUS_INVALID_HANDLE;
    auto* mutant = dynamic_cast<Mutant*>(GetKernelObject(handle));
    if (mutant == nullptr)
        return X_STATUS_INVALID_HANDLE;
    if (previousCount != nullptr)
        *previousCount = 0;
    try
    {
        mutant->mutex.unlock();
    }
    catch (...)
    {
        return X_STATUS_MUTANT_NOT_OWNED;
    }
    return X_STATUS_SUCCESS;
}

uint32_t NtYieldExecution()
{
    std::this_thread::yield();
    return X_STATUS_SUCCESS;
}

// SLIST_HEADER { Next (pointer), Depth (16), Sequence (16) }, big-endian; entries start with Next.
static std::mutex g_slistMutex;

uint32_t InterlockedPopEntrySList(be<uint32_t>* header)
{
    std::lock_guard lock(g_slistMutex);
    uint32_t entry = header[0];
    if (entry == 0)
        return 0;
    header[0] = *(be<uint32_t>*)g_memory.Translate(entry);
    auto* counts = (be<uint16_t>*)(header + 1);
    counts[0] = uint16_t(counts[0] - 1);
    counts[1] = uint16_t(counts[1] + 1);
    return entry;
}

// KeInitializeDpc(dpc, routine, context): KDPC { Type, ..., DeferredRoutine, DeferredContext, ... }.
uint32_t KeInitializeDpc(uint8_t* dpc, uint32_t routine, uint32_t context)
{
    if (dpc != nullptr)
    {
        memset(dpc, 0, 0x1C);
        *(be<uint16_t>*)dpc = 19; // DpcObject
        *(be<uint32_t>*)(dpc + 0x0C) = routine;
        *(be<uint32_t>*)(dpc + 0x10) = context;
    }
    return 0;
}

uint32_t MmMapIoSpace(uint32_t physical, uint32_t size, uint32_t flags) { return physical; }
uint32_t MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protect) { return 0; }

GUEST_FUNCTION_HOOK(__imp__XamUserGetName, XamUserGetName);
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege);
GUEST_FUNCTION_HOOK(__imp__XamUserAreUsersFriends, XamUserAreUsersFriends);
GUEST_FUNCTION_HOOK(__imp__XamUserGetMembershipTierFromXUID, XamUserGetMembershipTierFromXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserGetOnlineCountryFromXUID, XamUserGetOnlineCountryFromXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamCreateEnumeratorHandle, XamCreateEnumeratorHandle);
GUEST_FUNCTION_HOOK(__imp__XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle);
GUEST_FUNCTION_HOOK(__imp__XamShowAchievementsUI, XamShowUIStub);
GUEST_FUNCTION_HOOK(__imp__XamShowFriendsUI, XamShowUIStub);
GUEST_FUNCTION_HOOK(__imp__XamShowGamerCardUIForXUID, XamShowUIStub);
GUEST_FUNCTION_HOOK(__imp__XamShowMarketplaceUI, XamShowUIStub);
GUEST_FUNCTION_HOOK(__imp__XamShowMarketplaceDownloadItemsUI, XamShowUIStub);
GUEST_FUNCTION_HOOK(__imp__XamLoaderGetLaunchData, XamLoaderGetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamLoaderSetLaunchData, XamLoaderSetLaunchData);
GUEST_FUNCTION_HOOK(__imp__XamTaskSchedule, XamTaskSchedule);
GUEST_FUNCTION_HOOK(__imp__XamTaskShouldExit, XamTaskShouldExit);
GUEST_FUNCTION_HOOK(__imp__XamTaskCloseHandle, XamTaskCloseHandle);
GUEST_FUNCTION_HOOK(__imp__XamSessionCreateHandle, XamSessionCreateHandle);
GUEST_FUNCTION_HOOK(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle);
GUEST_FUNCTION_HOOK(__imp__XamVoiceCreate, XamVoiceCreate);
GUEST_FUNCTION_HOOK(__imp__XamVoiceClose, XamVoiceClose);
GUEST_FUNCTION_HOOK(__imp__XamVoiceHeadsetPresent, XamVoiceHeadsetPresent);
GUEST_FUNCTION_HOOK(__imp__XamVoiceSubmitPacket, XamVoiceSubmitPacket);
GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree);
GUEST_FUNCTION_HOOK(__imp__XamContentFlush, XamContentFlush);
GUEST_FUNCTION_HOOK(__imp__XNotifyPositionUI, XNotifyPositionUI);
GUEST_FUNCTION_HOOK(__imp__XMsgCancelIORequest, XMsgCancelIORequest);
GUEST_FUNCTION_HOOK(__imp__XNetLogonGetTitleID, XNetLogonGetTitleID);
GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);
GUEST_FUNCTION_HOOK(__imp__XeKeysConsolePrivateKeySign, XeKeysConsolePrivateKeySign);
GUEST_FUNCTION_HOOK(__imp__XeKeysConsoleSignatureVerification, XeKeysConsoleSignatureVerification);
GUEST_FUNCTION_HOOK(__imp__NtCreateMutant, NtCreateMutant);
GUEST_FUNCTION_HOOK(__imp__NtReleaseMutant, NtReleaseMutant);
GUEST_FUNCTION_HOOK(__imp__NtYieldExecution, NtYieldExecution);
GUEST_FUNCTION_HOOK(__imp__InterlockedPopEntrySList, InterlockedPopEntrySList);
GUEST_FUNCTION_HOOK(__imp__KeInitializeDpc, KeInitializeDpc);
GUEST_FUNCTION_HOOK(__imp__MmMapIoSpace, MmMapIoSpace);
GUEST_FUNCTION_HOOK(__imp__MmSetAddressProtect, MmSetAddressProtect);
