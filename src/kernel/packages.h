#pragma once

// The game's data packages (lu/*.lu, *.cu): a header and a payload stored either as is or
// as 1 MB chunks compressed with XMemCompress (LZX). Decompression calls the game's own
// (recompiled) XMemDecompress, so it must run on a thread with a guest context.
namespace packages
{
    // Reads a package's contents (its payload, decompressed). Returns false if the file
    // isn't a package or can't be decompressed.
    bool Read(const std::filesystem::path& path, std::vector<uint8_t>& contents);
}
