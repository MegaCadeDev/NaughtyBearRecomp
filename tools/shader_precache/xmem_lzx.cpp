// XMemCompress's LZX streams, decoded with libmspack's LZX decoder.
//
// Two differences from the CAB format libmspack implements:
//  - the stream is framed: each 32 KB frame's compressed bytes are preceded by their size
//    (big-endian 16 bits), and by 0xFF and the frame's uncompressed size when that isn't
//    32 KB; a size of 0 ends the stream. The frames' bytes, joined, are the LZX bitstream.
//  - an uncompressed block of odd length isn't followed by a padding byte. CMake builds a
//    copy of libmspack's lzxd.c without that step (xmem_lzxd.c), with its functions renamed.
#include "xmem_lzx.h"

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C"
{
#include <mspack.h>
#include <lzx.h>
}

namespace
{
    // A libmspack "file" over a memory buffer.
    struct MemoryFile
    {
        uint8_t* data;
        size_t length;
        size_t position;
    };

    mspack_file* Open(mspack_system*, const char* name, int) { return (mspack_file*)name; }
    void Close(mspack_file*) {}
    int Read(mspack_file* file, void* buffer, int bytes)
    {
        auto* memory = (MemoryFile*)file;
        size_t count = std::min<size_t>(size_t(bytes), memory->length - memory->position);
        memcpy(buffer, memory->data + memory->position, count);
        memory->position += count;
        return int(count);
    }
    int Write(mspack_file* file, void* buffer, int bytes)
    {
        auto* memory = (MemoryFile*)file;
        size_t count = std::min<size_t>(size_t(bytes), memory->length - memory->position);
        memcpy(memory->data + memory->position, buffer, count);
        memory->position += count;
        return int(count);
    }
    int Seek(mspack_file*, off_t, int) { return 0; }
    off_t Tell(mspack_file* file) { return off_t(((MemoryFile*)file)->position); }
    void Message(mspack_file*, const char*, ...) {}
    void* Allocate(mspack_system*, size_t bytes) { return malloc(bytes); }
    void Free(void* pointer) { free(pointer); }
    void Copy(void* source, void* destination, size_t bytes) { memmove(destination, source, bytes); }

    mspack_system g_system = { Open, Close, Read, Write, Seek, Tell, Message, Allocate, Free, Copy, nullptr };
}

bool XMemDecompressLzx(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t destinationSize, uint32_t windowSize)
{
    std::vector<uint8_t> stream;
    stream.reserve(sourceSize);
    size_t offset = 0;
    while (offset + 2 <= sourceSize)
    {
        uint32_t blockSize;
        if (source[offset] == 0xFF)
        {
            if (offset + 5 > sourceSize)
                return false;
            blockSize = (uint32_t(source[offset + 3]) << 8) | source[offset + 4];
            offset += 5;
        }
        else
        {
            blockSize = (uint32_t(source[offset]) << 8) | source[offset + 1];
            offset += 2;
        }
        if (blockSize == 0)
            break;
        if (offset + blockSize > sourceSize)
            return false;
        stream.insert(stream.end(), source + offset, source + offset + blockSize);
        offset += blockSize;
    }

    int windowBits = 0;
    while ((1u << windowBits) < windowSize)
        windowBits++;
    MemoryFile input{ stream.data(), stream.size(), 0 };
    MemoryFile output{ destination, destinationSize, 0 };
    lzxd_stream* lzx = xmem_lzxd_init(&g_system, (mspack_file*)&input, (mspack_file*)&output, windowBits, 0, 0x8000, off_t(destinationSize), 0);
    if (lzx == nullptr)
        return false;
    int result = xmem_lzxd_decompress(lzx, off_t(destinationSize));
    xmem_lzxd_free(lzx);
    return result == MSPACK_ERR_OK && output.position == destinationSize;
}
