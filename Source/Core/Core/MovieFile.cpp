// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MovieFile.h"

namespace Movie
{

bool IsMovieHeader(const std::array<u8, 4>& magic)
{
  return magic == std::array<u8, 4>{{'D', 'T', 'M', 0x1A}};
}

bool ReadDTMHeader(File::IOFile& file, DTMHeader* header)
{
  return file.ReadArray(header, 1);
}

bool WriteDTMHeader(File::IOFile& file, const DTMHeader& header)
{
  return file.WriteArray(&header, 1);
}

}  // namespace Movie
