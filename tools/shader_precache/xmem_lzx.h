// XMemDecompress for XMEMCODEC_LZX streams, without the game: what the precache tool uses to
// read the data packages.
#pragma once

#include <cstddef>
#include <cstdint>

// Decompresses an XMemCompress (LZX) stream of exactly destinationSize bytes.
bool XMemDecompressLzx(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t destinationSize, uint32_t windowSize);
