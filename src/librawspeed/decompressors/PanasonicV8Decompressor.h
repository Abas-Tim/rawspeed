/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2022-2024 LibRaw LLC (info@libraw.org)
    Copyright (C) 2024 Daniel Vogelbacher
    Copyright (C) 2025 Kolton Yager

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#pragma once

#include "common/RawImage.h"
#include "decompressors/AbstractDecompressor.h"
#include "tiff/TiffIFD.h"
#include <array>
#include <cstdint>
#include <vector>

namespace rawspeed {

/// Decompressor for Panasonic's RW2 version 8 format.
///
/// V8 is similar to lossless JPEG compression from the JPEG 92 spec and DNG
/// Each raw file is broken up into a number of separate strips, each of which
/// was separately encoded, and which can be decoded independently. For each
/// strip, an initial predicted value is provided. The strip's data buffer is
/// then decoded using the Huffman table provided in metadata. Each value
/// decoded from the strip is a difference between the predicted value and
/// actual value, allowing the actual value to be reconstructed.
class PanasonicV8Decompressor final : public AbstractDecompressor {
private:
  mutable RawImage mRawOutput;

public:
  /// Four values, one for each component of the sensor's color filter array.
  using Bayer2x2 = std::array<uint16_t, 4>;

  /// Decompressor parameters populated from tags. They remain constant after
  /// construction.
  struct DecompressorParams {
    std::vector<uint32_t> stripByteOffsets;
    std::vector<uint32_t> stripLineOffsets;
    std::vector<uint32_t> stripBitLengths;
    std::vector<uint16_t> stripWidths;
    std::vector<uint16_t> stripHeights;
    uint16_t horizontalStripCount;
    uint16_t verticalStripCount;

    Bayer2x2 initialPrediction;

    /// Huffman decoding shift down value. Appears to be unused.
    std::vector<uint16_t> huffShiftDown;

    uint16_t gammaClipVal;

    void validate() const;

    DecompressorParams() = delete;
    explicit DecompressorParams(const TiffIFD& ifd);
  };

  // Pre-cached Huffman decoded values for rapid lookup.
  struct HuffmanLUTEntry {
    uint8_t bitcount = 7;
    uint8_t diffCat = 0;
  };
  using HuffmanLUT = std::vector<HuffmanLUTEntry>;

private:
  const DecompressorParams mParams;
  HuffmanLUT mHuffmanLUT;

  std::vector<Array1DRef<const uint8_t>> mStrips;

  /// Huffman decoder helper class. Defined only in the cpp file.
  class InternalHuffDecoder;

  /// Thread safe function for decompressing a single data-stripstrip within a
  /// Rw2V8 raw image.
  void decompressStrip(unsigned stripIdx, InternalHuffDecoder decoder,
                       Array2DRef<uint16_t> outBuffer) const;

public:
  PanasonicV8Decompressor(Buffer inputFile, RawImage outputImg,
                          DecompressorParams mParams_, HuffmanLUT mHuffmanLUT_,
                          std::vector<Array1DRef<const uint8_t>> mStrips_);

  /// Run the decompressor on the provided raw image
  void decompress() const;
};

} // namespace rawspeed
