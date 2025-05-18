/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2025 Roman Lebedev

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "decompressors/PanasonicV8Decompressor.h"
#include "MemorySanitizer.h"
#include "adt/Array1DRef.h"
#include "adt/Array1DRefExtras.h"
#include "adt/Array2DRef.h"
#include "adt/Casts.h"
#include "adt/CroppedArray2DRef.h"
#include "adt/Invariant.h"
#include "common/RawImage.h"
#include "common/RawspeedException.h"
#include "fuzz/Common.h"
#include "io/Buffer.h"
#include "io/ByteStream.h"
#include "io/Endianness.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size);

namespace rawspeed {

namespace {

ByteStream getTrailingStrips(ByteStream bs, ByteStream sizesBs,
                             std::vector<Array1DRef<const uint8_t>>* out) {
  invariant(sizesBs.getRemainSize() % sizeof(uint32_t) == 0);
  uint32_t numStrips = sizesBs.getRemainSize() / sizeof(uint32_t);

  if (out)
    out->reserve(numStrips);
  for (uint32_t strip = 0; strip != numStrips; ++strip) {
    const uint32_t stripSize = sizesBs.getU32();
    Buffer buf = bs.getBuffer(stripSize);
    if (out)
      out->emplace_back(buf);
  }
  invariant(sizesBs.getRemainSize() == 0);
  return bs;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
  assert(Data);

  try {
    const Buffer b(Data, implicit_cast<Buffer::size_type>(Size));
    const DataBuffer db(b, Endianness::little);
    ByteStream bs(db);

    RawImage mRaw(CreateRawImage(bs));

    uint32_t numStrips = bs.getU32();
    uint32_t numOutTiles = bs.getU32();
    uint32_t numHuffmanLUTEntries = bs.getU32();

    auto stripSizes = bs.getStream(numStrips, sizeof(uint32_t));
    auto outTileSizes = bs.getStream(numOutTiles, 4 * sizeof(int32_t));
    auto huffmanLUTEntriesInput =
        bs.getStream(numHuffmanLUTEntries, 2 * sizeof(uint8_t));
    const auto initialPrediction = bs.getArray<uint16_t, 4>();

    // The rest of the bs are the input strips.

    getTrailingStrips(bs, stripSizes, nullptr);
    // If we have not run out of tmp, we're good to proceed.
    std::vector<Array1DRef<const uint8_t>> strips;
    try {
      bs = getTrailingStrips(bs, stripSizes, &strips);
    } catch (...) {
      __builtin_unreachable();
    }
    bs = {}; // bs is no longer needed.

    mRaw->createData();

    std::vector<Array2DRef<uint16_t>> outTiles;
    outTiles.reserve(numOutTiles);
    for (uint32_t stripIdx = 0; stripIdx < numOutTiles; ++stripIdx) {
      const auto stripWidth = outTileSizes.get<int32_t>();
      const auto stripHeight = outTileSizes.get<int32_t>();
      const auto stripOutputX = outTileSizes.get<int32_t>();
      const auto stripOutputY = outTileSizes.get<int32_t>();

      const auto out =
          CroppedArray2DRef<uint16_t>(mRaw->getU16DataAsUncroppedArray2DRef(),
                                      /*offsetCols=*/stripOutputX,
                                      /*offsetRows=*/stripOutputY,
                                      /*croppedWidth=*/stripWidth,
                                      /*croppedHeight=*/stripHeight)
              .getAsArray2DRef();

      outTiles.emplace_back(out);
    }
    invariant(outTileSizes.getRemainSize() == 0);

    std::vector<PanasonicV8Decompressor::HuffmanLUTEntry> huffmanLUT;
    huffmanLUT.reserve(numHuffmanLUTEntries);
    for (uint32_t entryIdx = 0; entryIdx < numHuffmanLUTEntries; ++entryIdx) {
      const auto bitcount = huffmanLUTEntriesInput.get<uint8_t>();
      const auto diffCat = huffmanLUTEntriesInput.get<uint8_t>();

      huffmanLUT.emplace_back(PanasonicV8Decompressor::HuffmanLUTEntry{
          .bitcount = bitcount, .diffCat = diffCat});
    }
    invariant(huffmanLUTEntriesInput.getRemainSize() == 0);

    PanasonicV8Decompressor::DecompressorParams params{
        .mStrips = getAsArray1DRef(strips),
        .mOutTiles = getAsArray1DRef(outTiles),
        .initialPrediction = initialPrediction,
    };

    PanasonicV8Decompressor v8(mRaw, params, getAsArray1DRef(huffmanLUT));
    v8.decompress();
    MSan::CheckMemIsInitialized(mRaw->getByteDataAsUncroppedArray2DRef());
  } catch (const RawspeedException&) { // NOLINT(bugprone-empty-catch)
    // Exceptions are good, crashes are bad.
  }

  return 0;
}

} // namespace rawspeed
