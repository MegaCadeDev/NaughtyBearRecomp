// Kernel-level file I/O (NtCreateFile, NtReadFile, ...) for Naughty Bear.
//
// Unleashed Recompiled hooks Sonic's own CreateFileA-style functions by address instead,
// so this layer is new. Requests complete synchronously; when the caller passes an
// event it is signalled, so asynchronous-style reads behave as already finished.
//
// Xbox paths are case-insensitive and Linux file systems are not, so every path is
// resolved component by component against the real directory contents.

#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <os/logger.h>
#include <user/paths.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

void SignalEventHandle(uint32_t handle);

namespace
{
    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;
    constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
    constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;

    // CreateDisposition
    constexpr uint32_t FILE_SUPERSEDE = 0;
    constexpr uint32_t FILE_OPEN = 1;
    constexpr uint32_t FILE_CREATE = 2;
    constexpr uint32_t FILE_OPEN_IF = 3;
    constexpr uint32_t FILE_OVERWRITE = 4;
    constexpr uint32_t FILE_OVERWRITE_IF = 5;

    // CreateOptions
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
    constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;

    // IoStatusBlock.Information after a create
    constexpr uint32_t FILE_SUPERSEDED = 0;
    constexpr uint32_t FILE_OPENED = 1;
    constexpr uint32_t FILE_CREATED = 2;
    constexpr uint32_t FILE_OVERWRITTEN = 3;

    constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01;
    constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
    constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

    constexpr uint32_t ACCESS_WRITE_MASK = 0x40000000 /* GENERIC_WRITE */ | 0x10000000 /* GENERIC_ALL */ | 0x0002 /* FILE_WRITE_DATA */ | 0x0004 /* FILE_APPEND_DATA */;

    struct FileHandle final : KernelObject
    {
        int fd = -1;
        bool isDirectory = false;
        std::filesystem::path path;
        uint64_t position = 0;

        // Directory enumeration state for NtQueryDirectoryFile.
        std::vector<std::filesystem::path> entries;
        size_t nextEntry = 0;
        bool enumerated = false;

        ~FileHandle() override
        {
            if (fd >= 0)
                close(fd);
        }
    };

    Mutex g_pathCacheMutex;
    ankerl::unordered_dense::map<std::string, std::filesystem::path> g_pathCache;

    std::string ToLower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    }

    // Resolves a relative path one component at a time, matching names case-insensitively.
    // Components that do not exist yet are appended as given (for files being created).
    std::filesystem::path ResolveCaseInsensitive(const std::filesystem::path& root, std::string_view relative)
    {
        std::string key = ToLower(root.string()) + '|' + ToLower(relative);
        {
            std::lock_guard lock(g_pathCacheMutex);
            if (auto it = g_pathCache.find(key); it != g_pathCache.end())
                return it->second;
        }

        std::filesystem::path current = root;
        bool missing = false;
        size_t start = 0;
        while (start <= relative.size())
        {
            size_t end = relative.find('/', start);
            if (end == std::string_view::npos)
                end = relative.size();
            std::string_view part = relative.substr(start, end - start);
            start = end + 1;

            if (part.empty() || part == ".")
                continue;
            if (part == "..")
            {
                current = current.parent_path();
                continue;
            }

            std::error_code ec;
            std::filesystem::path exact = current / part;
            if (!missing && !std::filesystem::exists(exact, ec))
            {
                bool found = false;
                std::string wanted = ToLower(part);
                for (const auto& entry : std::filesystem::directory_iterator(current, ec))
                {
                    if (ToLower(entry.path().filename().string()) == wanted)
                    {
                        current = entry.path();
                        found = true;
                        break;
                    }
                }
                if (found)
                    continue;
                missing = true;
            }
            current = exact;
        }

        if (!missing)
        {
            std::lock_guard lock(g_pathCacheMutex);
            g_pathCache.emplace(std::move(key), current);
        }
        return current;
    }

    void ForgetCachedPaths()
    {
        std::lock_guard lock(g_pathCacheMutex);
        g_pathCache.clear();
    }

    // Maps an Xbox object path ("game:\a\b", "\Device\Cdrom0\a", "\??\D:\a") to a host path.
    std::filesystem::path ResolveGuestPath(std::string_view guestPath, const FileHandle* rootDirectory)
    {
        std::string path(guestPath);
        std::replace(path.begin(), path.end(), '\\', '/');

        if (rootDirectory != nullptr)
            return ResolveCaseInsensitive(rootDirectory->path, path);

        if (path.starts_with("/??/"))
            path = path.substr(4);

        std::string lower = ToLower(path);
        constexpr std::pair<std::string_view, std::string_view> devices[] =
        {
            { "/device/cdrom0", "game" },
            { "/device/harddisk0/partition1", "save" },
        };
        for (auto [device, root] : devices)
        {
            if (lower.starts_with(device) && (lower.size() == device.size() || lower[device.size()] == '/'))
            {
                std::filesystem::path base = root == "save" ? GetSavePath(false) : GetGamePath();
                return ResolveCaseInsensitive(base, path.substr(device.size()));
            }
        }

        size_t colon = path.find(':');
        if (colon != std::string::npos)
        {
            std::string root = path.substr(0, colon);
            std::string_view mapped = XamGetRootPath(root);
            if (mapped.empty())
                mapped = XamGetRootPath(ToLower(root));
            std::filesystem::path base = mapped.empty() ? GetGamePath() : std::filesystem::path(std::string(mapped));
            if (mapped.empty())
                LOGF_WARNING("Unknown root \"{}:\" in \"{}\", using the game folder", root, guestPath);
            return ResolveCaseInsensitive(base, path.substr(colon + 1));
        }

        return ResolveCaseInsensitive(GetGamePath(), path);
    }

    std::string_view GuestString(const XANSI_STRING* str)
    {
        if (str == nullptr || str->Buffer.get() == nullptr)
            return {};
        return { str->Buffer.get(), str->Length.get() };
    }

    // Pseudo-handles at the top of the range: -1 invalid, -2 current thread, and -3, which
    // object attributes use as RootDirectory to mean "no root directory".
    constexpr uint32_t PSEUDO_HANDLE_BEGIN = 0xFFFFFF00;

    FileHandle* GetFileHandle(uint32_t handle)
    {
        if (handle == 0 || handle >= PSEUDO_HANDLE_BEGIN || !IsKernelObject(handle))
            return nullptr;
        return dynamic_cast<FileHandle*>(GetKernelObject(handle));
    }

    void Complete(XIO_STATUS_BLOCK* ioStatusBlock, uint32_t status, uint32_t information, uint32_t event = 0)
    {
        if (ioStatusBlock != nullptr)
        {
            ioStatusBlock->Status = status;
            ioStatusBlock->Information = information;
        }
        SignalEventHandle(event);
    }

    // Windows FILETIME (100 ns since 1601) from a host file time.
    uint64_t ToFileTime(const struct timespec& ts)
    {
        constexpr uint64_t EPOCH_DIFFERENCE = 116444736000000000ull;
        return EPOCH_DIFFERENCE + uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
    }

    struct X_FILE_NETWORK_OPEN_INFORMATION
    {
        be<uint64_t> CreationTime;
        be<uint64_t> LastAccessTime;
        be<uint64_t> LastWriteTime;
        be<uint64_t> ChangeTime;
        be<uint64_t> AllocationSize;
        be<uint64_t> EndOfFile;
        be<uint32_t> FileAttributes;
        be<uint32_t> Pad;
    };
    static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);

    bool FillNetworkOpenInfo(const std::filesystem::path& path, X_FILE_NETWORK_OPEN_INFORMATION* info)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return false;
        bool dir = S_ISDIR(st.st_mode);
        info->CreationTime = ToFileTime(st.st_ctim);
        info->LastAccessTime = ToFileTime(st.st_atim);
        info->LastWriteTime = ToFileTime(st.st_mtim);
        info->ChangeTime = ToFileTime(st.st_mtim);
        info->AllocationSize = dir ? 0 : RoundUp<uint64_t>(st.st_size, 0x1000);
        info->EndOfFile = dir ? 0 : st.st_size;
        info->FileAttributes = dir ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
        info->Pad = 0;
        return true;
    }

    // Wildcard match for NtQueryDirectoryFile masks (* and ?), case-insensitive.
    bool MatchMask(std::string_view name, std::string_view mask)
    {
        if (mask.empty() || mask == "*" || mask == "*.*")
            return true;
        size_t n = 0, m = 0, starM = std::string_view::npos, starN = 0;
        while (n < name.size())
        {
            if (m < mask.size() && (mask[m] == '?' || std::tolower((unsigned char)mask[m]) == std::tolower((unsigned char)name[n])))
            {
                n++;
                m++;
            }
            else if (m < mask.size() && mask[m] == '*')
            {
                starM = m++;
                starN = n;
            }
            else if (starM != std::string_view::npos)
            {
                m = starM + 1;
                n = ++starN;
            }
            else
            {
                return false;
            }
        }
        while (m < mask.size() && mask[m] == '*')
            m++;
        return m == mask.size();
    }
}

uint32_t NtCreateFile(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatusBlock,
    be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess, uint32_t createDisposition, uint32_t createOptions)
{
    if (fileHandle == nullptr || attributes == nullptr)
        return X_STATUS_INVALID_PARAMETER;

    std::string_view guestPath = GuestString(attributes->Name.get());
    FileHandle* rootDirectory = GetFileHandle(attributes->RootDirectory);
    std::filesystem::path path = ResolveGuestPath(guestPath, rootDirectory);

    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);
    bool isDirectory = exists && std::filesystem::is_directory(path, ec);
    bool wantsWrite = (desiredAccess & ACCESS_WRITE_MASK) != 0;
    uint32_t information = FILE_OPENED;

    auto fail = [&](uint32_t status)
        {
            LOGF_IMPL(Warning, "NtCreateFile", "\"{}\" -> {}: 0x{:08X}", guestPath, path.string(), status);
            *fileHandle = GUEST_INVALID_HANDLE_VALUE;
            Complete(ioStatusBlock, status, 0);
            return status;
        };

    if ((createOptions & FILE_DIRECTORY_FILE) != 0)
    {
        if (!exists)
        {
            if (createDisposition == FILE_OPEN || createDisposition == FILE_OVERWRITE)
                return fail(X_STATUS_OBJECT_NAME_NOT_FOUND);
            if (!std::filesystem::create_directories(path, ec))
                return fail(X_STATUS_OBJECT_PATH_NOT_FOUND);
            ForgetCachedPaths();
            information = FILE_CREATED;
        }
        else if (!isDirectory)
        {
            return fail(X_STATUS_NOT_A_DIRECTORY);
        }
        else if (createDisposition == FILE_CREATE)
        {
            return fail(X_STATUS_OBJECT_NAME_COLLISION);
        }
        isDirectory = true;
    }
    else if (isDirectory)
    {
        if ((createOptions & FILE_NON_DIRECTORY_FILE) != 0)
            return fail(X_STATUS_ACCESS_DENIED);
    }
    else
    {
        int flags = wantsWrite ? O_RDWR : O_RDONLY;
        switch (createDisposition)
        {
        case FILE_OPEN:
            if (!exists)
                return fail(std::filesystem::exists(path.parent_path(), ec) ? X_STATUS_OBJECT_NAME_NOT_FOUND : X_STATUS_OBJECT_PATH_NOT_FOUND);
            break;
        case FILE_CREATE:
            if (exists)
                return fail(X_STATUS_OBJECT_NAME_COLLISION);
            flags |= O_CREAT | O_EXCL;
            information = FILE_CREATED;
            break;
        case FILE_OPEN_IF:
            flags |= O_CREAT;
            information = exists ? FILE_OPENED : FILE_CREATED;
            break;
        case FILE_OVERWRITE:
            if (!exists)
                return fail(X_STATUS_OBJECT_NAME_NOT_FOUND);
            flags |= O_TRUNC;
            information = FILE_OVERWRITTEN;
            break;
        case FILE_SUPERSEDE:
        case FILE_OVERWRITE_IF:
            flags |= O_CREAT | O_TRUNC;
            information = exists ? (createDisposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN) : FILE_CREATED;
            break;
        default:
            return fail(X_STATUS_INVALID_PARAMETER);
        }

        if ((flags & (O_CREAT | O_TRUNC)) != 0)
            flags = (flags & ~O_ACCMODE) | O_RDWR;

        if ((flags & O_CREAT) != 0)
            std::filesystem::create_directories(path.parent_path(), ec);

        int fd = open(path.c_str(), flags, 0644);
        if (fd < 0)
            return fail(errno == ENOENT ? X_STATUS_OBJECT_PATH_NOT_FOUND : X_STATUS_ACCESS_DENIED);
        if (!exists)
            ForgetCachedPaths();

        static const bool logFiles = getenv("NB_LOG_FILES") != nullptr;
        if (logFiles)
            LOGF_IMPL(Utility, "NtCreateFile", "\"{}\" -> {}", guestPath, path.string());

        auto* handle = CreateKernelObject<FileHandle>();
        handle->fd = fd;
        handle->path = path;
        *fileHandle = GetKernelHandle(handle);
        Complete(ioStatusBlock, X_STATUS_SUCCESS, information);
        return X_STATUS_SUCCESS;
    }

    auto* handle = CreateKernelObject<FileHandle>();
    handle->isDirectory = true;
    handle->path = path;
    *fileHandle = GetKernelHandle(handle);
    Complete(ioStatusBlock, X_STATUS_SUCCESS, information);
    return X_STATUS_SUCCESS;
}

uint32_t NtOpenFile(be<uint32_t>* fileHandle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatusBlock,
    uint32_t shareAccess, uint32_t openOptions)
{
    return NtCreateFile(fileHandle, desiredAccess, attributes, ioStatusBlock, nullptr, 0, shareAccess, FILE_OPEN, openOptions);
}

uint32_t NtReadFile(uint32_t fileHandle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatusBlock,
    void* buffer, uint32_t length, be<int64_t>* byteOffset)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr || handle->fd < 0)
        return X_STATUS_INVALID_HANDLE;

    uint64_t offset = byteOffset != nullptr && *byteOffset >= 0 ? uint64_t(int64_t(*byteOffset)) : handle->position;
    ssize_t read = pread(handle->fd, buffer, length, offset);
    if (read < 0)
        read = 0;
    handle->position = offset + read;

    uint32_t status = (read == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    Complete(ioStatusBlock, status, uint32_t(read), event);
    if (apcRoutine != 0)
        LOG_UTILITY("APC completion routines are not supported");
    return status;
}

uint32_t NtReadFileScatter(uint32_t fileHandle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatusBlock,
    be<uint32_t>* segmentArray, uint32_t length, be<int64_t>* byteOffset)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr || handle->fd < 0 || segmentArray == nullptr)
        return X_STATUS_INVALID_HANDLE;

    // The segment array lists one 4 KB guest page per element.
    uint64_t offset = byteOffset != nullptr && *byteOffset >= 0 ? uint64_t(int64_t(*byteOffset)) : handle->position;
    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        uint32_t chunk = std::min<uint32_t>(0x1000, length - total);
        ssize_t read = pread(handle->fd, g_memory.Translate(segmentArray[i]), chunk, offset + total);
        if (read <= 0)
            break;
        total += uint32_t(read);
        if (uint32_t(read) < chunk)
            break;
    }
    handle->position = offset + total;

    uint32_t status = (total == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    Complete(ioStatusBlock, status, total, event);
    return status;
}

uint32_t NtWriteFile(uint32_t fileHandle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatusBlock,
    const void* buffer, uint32_t length, be<int64_t>* byteOffset)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr || handle->fd < 0)
        return X_STATUS_INVALID_HANDLE;

    uint64_t offset = byteOffset != nullptr && *byteOffset >= 0 ? uint64_t(int64_t(*byteOffset)) : handle->position;
    ssize_t written = pwrite(handle->fd, buffer, length, offset);
    if (written < 0)
    {
        Complete(ioStatusBlock, X_STATUS_ACCESS_DENIED, 0, event);
        return X_STATUS_ACCESS_DENIED;
    }
    handle->position = offset + written;
    Complete(ioStatusBlock, X_STATUS_SUCCESS, uint32_t(written), event);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatusBlock, void* fileInformation, uint32_t length, uint32_t fileInformationClass)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr)
        return X_STATUS_INVALID_HANDLE;

    struct stat st{};
    if (handle->fd >= 0)
        fstat(handle->fd, &st);
    else
        stat(handle->path.c_str(), &st);

    auto require = [&](uint32_t size)
        {
            if (length < size)
            {
                Complete(ioStatusBlock, X_STATUS_INFO_LENGTH_MISMATCH, 0);
                return false;
            }
            memset(fileInformation, 0, size);
            return true;
        };

    switch (fileInformationClass)
    {
    case 4: // FileBasicInformation
    {
        struct Info { be<uint64_t> CreationTime, LastAccessTime, LastWriteTime, ChangeTime; be<uint32_t> FileAttributes, Pad; };
        if (!require(sizeof(Info)))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        auto* info = (Info*)fileInformation;
        info->CreationTime = ToFileTime(st.st_ctim);
        info->LastAccessTime = ToFileTime(st.st_atim);
        info->LastWriteTime = ToFileTime(st.st_mtim);
        info->ChangeTime = ToFileTime(st.st_mtim);
        info->FileAttributes = handle->isDirectory ? X_FILE_ATTRIBUTE_DIRECTORY : X_FILE_ATTRIBUTE_NORMAL;
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(Info));
        return X_STATUS_SUCCESS;
    }
    case 5: // FileStandardInformation
    {
        struct Info { be<uint64_t> AllocationSize, EndOfFile; be<uint32_t> NumberOfLinks; uint8_t DeletePending, Directory; uint8_t Pad[2]; };
        if (!require(sizeof(Info)))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        auto* info = (Info*)fileInformation;
        info->AllocationSize = RoundUp<uint64_t>(st.st_size, 0x1000);
        info->EndOfFile = st.st_size;
        info->NumberOfLinks = 1;
        info->Directory = handle->isDirectory;
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(Info));
        return X_STATUS_SUCCESS;
    }
    case 6: // FileInternalInformation
    {
        if (!require(8))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)fileInformation = uint64_t(st.st_ino);
        Complete(ioStatusBlock, X_STATUS_SUCCESS, 8);
        return X_STATUS_SUCCESS;
    }
    case 14: // FilePositionInformation
    {
        if (!require(8))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        *(be<uint64_t>*)fileInformation = handle->position;
        Complete(ioStatusBlock, X_STATUS_SUCCESS, 8);
        return X_STATUS_SUCCESS;
    }
    case 34: // FileNetworkOpenInformation
    {
        if (!require(sizeof(X_FILE_NETWORK_OPEN_INFORMATION)))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        FillNetworkOpenInfo(handle->path, (X_FILE_NETWORK_OPEN_INFORMATION*)fileInformation);
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(X_FILE_NETWORK_OPEN_INFORMATION));
        return X_STATUS_SUCCESS;
    }
    default:
        LOGF_WARNING("Unhandled information class {}", fileInformationClass);
        Complete(ioStatusBlock, X_STATUS_NOT_IMPLEMENTED, 0);
        return X_STATUS_NOT_IMPLEMENTED;
    }
}

uint32_t NtSetInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatusBlock, void* fileInformation, uint32_t length, uint32_t fileInformationClass)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr)
        return X_STATUS_INVALID_HANDLE;

    switch (fileInformationClass)
    {
    case 14: // FilePositionInformation
        handle->position = *(be<uint64_t>*)fileInformation;
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
        if (handle->fd >= 0 && ftruncate(handle->fd, off_t(uint64_t(*(be<uint64_t>*)fileInformation))) != 0)
        {
            Complete(ioStatusBlock, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
        break;
    case 13: // FileDispositionInformation
        if (*(uint8_t*)fileInformation)
        {
            std::error_code ec;
            std::filesystem::remove(handle->path, ec);
            ForgetCachedPaths();
        }
        break;
    case 4: // FileBasicInformation (times/attributes); nothing to keep
        break;
    default:
        LOGF_WARNING("Unhandled information class {}", fileInformationClass);
        Complete(ioStatusBlock, X_STATUS_NOT_IMPLEMENTED, 0);
        return X_STATUS_NOT_IMPLEMENTED;
    }

    Complete(ioStatusBlock, X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    if (attributes == nullptr || info == nullptr)
        return X_STATUS_INVALID_PARAMETER;

    FileHandle* rootDirectory = GetFileHandle(attributes->RootDirectory);
    std::filesystem::path path = ResolveGuestPath(GuestString(attributes->Name.get()), rootDirectory);
    if (!FillNetworkOpenInfo(path, info))
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    return X_STATUS_SUCCESS;
}

uint32_t NtQueryVolumeInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatusBlock, void* fsInformation, uint32_t length, uint32_t fsInformationClass)
{
    if (GetFileHandle(fileHandle) == nullptr)
        return X_STATUS_INVALID_HANDLE;

    switch (fsInformationClass)
    {
    case 1: // FileFsVolumeInformation
    {
        struct Info { be<uint64_t> VolumeCreationTime; be<uint32_t> VolumeSerialNumber, VolumeLabelLength; uint8_t SupportsObjects; char VolumeLabel[3]; };
        if (length < sizeof(Info))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        memset(fsInformation, 0, sizeof(Info));
        ((Info*)fsInformation)->VolumeSerialNumber = 0x4E425231; // "NBR1"
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(Info));
        return X_STATUS_SUCCESS;
    }
    case 3: // FileFsSizeInformation
    {
        struct Info { be<uint64_t> TotalAllocationUnits, AvailableAllocationUnits; be<uint32_t> SectorsPerAllocationUnit, BytesPerSector; };
        if (length < sizeof(Info))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        auto* info = (Info*)fsInformation;
        info->TotalAllocationUnits = 0x200000; // 8 GB in 4 KB units
        info->AvailableAllocationUnits = 0x100000;
        info->SectorsPerAllocationUnit = 8;
        info->BytesPerSector = 512;
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(Info));
        return X_STATUS_SUCCESS;
    }
    case 5: // FileFsAttributeInformation
    {
        struct Info { be<uint32_t> FileSystemAttributes, MaximumComponentNameLength, FileSystemNameLength; char FileSystemName[8]; };
        if (length < sizeof(Info))
            return X_STATUS_INFO_LENGTH_MISMATCH;
        auto* info = (Info*)fsInformation;
        memset(info, 0, sizeof(Info));
        info->MaximumComponentNameLength = 255;
        info->FileSystemNameLength = 4;
        memcpy(info->FileSystemName, "FATX", 4);
        Complete(ioStatusBlock, X_STATUS_SUCCESS, sizeof(Info));
        return X_STATUS_SUCCESS;
    }
    default:
        LOGF_WARNING("Unhandled volume information class {}", fsInformationClass);
        Complete(ioStatusBlock, X_STATUS_NOT_IMPLEMENTED, 0);
        return X_STATUS_NOT_IMPLEMENTED;
    }
}

uint32_t NtQueryDirectoryFile(uint32_t fileHandle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatusBlock,
    void* fileInformation, uint32_t length, XANSI_STRING* fileMask, uint32_t restartScan)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr || !handle->isDirectory)
        return X_STATUS_INVALID_HANDLE;

    if (!handle->enumerated || restartScan)
    {
        handle->entries.clear();
        handle->nextEntry = 0;
        std::string mask(GuestString(fileMask));
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(handle->path, ec))
        {
            if (MatchMask(entry.path().filename().string(), mask))
                handle->entries.push_back(entry.path());
        }
        std::sort(handle->entries.begin(), handle->entries.end());
        handle->enumerated = true;
    }

    // FILE_DIRECTORY_INFORMATION with an ANSI file name, one entry per call.
    struct Info
    {
        be<uint32_t> NextEntryOffset;
        be<uint32_t> FileIndex;
        be<uint64_t> CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
        be<uint64_t> EndOfFile, AllocationSize;
        be<uint32_t> FileAttributes;
        be<uint32_t> FileNameLength;
        char FileName[1];
    };

    if (handle->nextEntry >= handle->entries.size())
    {
        Complete(ioStatusBlock, X_STATUS_NO_MORE_FILES, 0, event);
        return X_STATUS_NO_MORE_FILES;
    }

    const auto& path = handle->entries[handle->nextEntry];
    std::string name = path.filename().string();
    uint32_t size = uint32_t(offsetof(Info, FileName) + name.size());
    if (length < size)
    {
        Complete(ioStatusBlock, X_STATUS_INFO_LENGTH_MISMATCH, 0, event);
        return X_STATUS_INFO_LENGTH_MISMATCH;
    }

    X_FILE_NETWORK_OPEN_INFORMATION open{};
    FillNetworkOpenInfo(path, &open);

    auto* info = (Info*)fileInformation;
    memset(info, 0, offsetof(Info, FileName));
    info->FileIndex = uint32_t(handle->nextEntry);
    info->CreationTime = open.CreationTime;
    info->LastAccessTime = open.LastAccessTime;
    info->LastWriteTime = open.LastWriteTime;
    info->ChangeTime = open.ChangeTime;
    info->EndOfFile = open.EndOfFile;
    info->AllocationSize = open.AllocationSize;
    info->FileAttributes = open.FileAttributes;
    info->FileNameLength = uint32_t(name.size());
    memcpy(info->FileName, name.data(), name.size());
    handle->nextEntry++;

    Complete(ioStatusBlock, X_STATUS_SUCCESS, size, event);
    return X_STATUS_SUCCESS;
}

uint32_t NtFlushBuffersFile(uint32_t fileHandle, XIO_STATUS_BLOCK* ioStatusBlock)
{
    auto* handle = GetFileHandle(fileHandle);
    if (handle == nullptr)
        return X_STATUS_INVALID_HANDLE;
    if (handle->fd >= 0)
        fsync(handle->fd);
    Complete(ioStatusBlock, X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFileScatter, NtReadFileScatter);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
