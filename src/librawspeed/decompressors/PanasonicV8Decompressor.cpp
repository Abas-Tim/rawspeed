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

#include "rawspeedconfig.h"
#include "decompressors/PanasonicV8Decompressor.h"
#include "adt/Array1DRef.h"
#include "adt/Array2DRef.h"
#include "adt/CroppedArray2DRef.h"
#include "adt/Invariant.h"
#include "bitstreams/BitStream.h"
#include "bitstreams/BitStreamer.h"
#include "bitstreams/BitStreamerMSB.h" // IWYU pragma: keep
#include "bitstreams/BitStreams.h"
#include "common/Common.h"
#include "common/RawImage.h"
#include "common/RawspeedException.h"
#include "decoders/RawDecoderException.h"
#include "io/IOException.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace rawspeed {

// The set of templates and classes below define a specialized bit streamer
// which works mostly the same as BitStreamerMSB, but which reverses the
// ordering of the bits within each byte.
template <typename Tag>
struct BitStreamerReversedSequentialReplenisher
    : public BitStreamerForwardSequentialReplenisher<Tag> {
  using Base = BitStreamerForwardSequentialReplenisher<Tag>;
  using Traits = BitStreamerTraits<Tag>;
  using StreamTraits = BitStreamTraits<Traits::Tag>;

  using Base::BitStreamerForwardSequentialReplenisher;

  // Almost an exact copy of
  // BitStreamerForwardSequentialReplenisher::getInput(), but here we flip the
  // order of the bits within each byte, as they get loaded.
  std::array<std::byte, BitStreamerTraits<Tag>::MaxProcessBytes> getInput() {
    Base::establishClassInvariants();

    std::array<std::byte, BitStreamerTraits<Tag>::MaxProcessBytes> tmpStorage;
    auto tmp = Array1DRef<std::byte>(tmpStorage.data(),
                                     implicit_cast<int>(tmpStorage.size()));

    if (Base::getPos() + BitStreamerTraits<Tag>::MaxProcessBytes <=
        Base::input.size()) [[likely]] {
      auto currInput =
          Base::input
              .getCrop(Base::getPos(), BitStreamerTraits<Tag>::MaxProcessBytes)
              .getAsArray1DRef();
      invariant(currInput.size() == tmp.size());
      std::copy_n(currInput.begin(), BitStreamerTraits<Tag>::MaxProcessBytes,
                  tmp.begin());

      // Reverse the order of bits within each byte using a bit-twiddle trick.
      // Three operation bit reversal from:
      // https://graphics.stanford.edu/~seander/bithacks.html#ReverseByteWith64BitsDiv
      for (std::byte& b : tmp) {
        b = std::byte{
            uint8_t((uint8_t(b) * 0x0202020202ULL & 0x010884422010ULL) % 1023)};
      }

      return tmpStorage;
    }

    if (Base::getPos() >
        Base::input.size() + 2 * BitStreamerTraits<Tag>::MaxProcessBytes)
        [[unlikely]]
      ThrowIOE("Buffer overflow read in BitStreamer");

    variableLengthLoadNaiveViaMemcpy(tmp, Base::input, Base::getPos());

    return tmpStorage;
  }
};

class BitStreamerRevMSB;

template <> struct BitStreamerTraits<BitStreamerRevMSB> final {
  static constexpr BitOrder Tag = BitOrder::MSB;

  static constexpr bool canUseWithPrefixCodeDecoder = true;

  static constexpr int MaxProcessBytes = 4;
  static_assert(MaxProcessBytes == sizeof(uint32_t));
};

/// Variation of standard MSB bit streamer. Bits are processed in reverse order
/// (least significant bits consume first).
class BitStreamerRevMSB final
    : public BitStreamer<
          BitStreamerRevMSB,
          BitStreamerReversedSequentialReplenisher<BitStreamerRevMSB>> {
  using Base =
      BitStreamer<BitStreamerRevMSB,
                  BitStreamerReversedSequentialReplenisher<BitStreamerRevMSB>>;

public:
  using Base::Base;
};

/// Utility class for Panasonic V8 entropy decoding
class PanasonicV8Decompressor::InternalHuffDecoder {
private:
  const Array1DRef<const HuffmanLUTEntry>
      mLUT; // Reference to PanasonicV8Decompressor::mHuffmanLUT
  BitStreamerRevMSB mBitPump;

public:
  InternalHuffDecoder(const Array1DRef<const HuffmanLUTEntry>& LUT,
                      Array1DRef<const uint8_t> bitStream)
      : mLUT(LUT), mBitPump(bitStream) {}

  int32_t decodeNextDiffValue();
};

void PanasonicV8Decompressor::DecompressorParams::validate() const {
  const int totalStrips = horizontalStripCount * verticalStripCount;

  if (totalStrips > mStrips.size())
    ThrowRDE("Strip byte buffer array does not have enough entries for the "
             "number of strips!");
  if (totalStrips > mOutTiles.size())
    ThrowRDE("Strip byte buffer array does not have enough entries for the "
             "number of strips!");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

PanasonicV8Decompressor::PanasonicV8Decompressor(
    RawImage outputImg, DecompressorParams mParams_,
    Array1DRef<const HuffmanLUTEntry> mHuffmanLUT_)
    : mRawOutput(std::move(outputImg)), mParams(std::move(mParams_)),
      mHuffmanLUT(mHuffmanLUT_) {
  if (mRawOutput->getCpp() != 1 ||
      mRawOutput->getDataType() != RawImageType::UINT16 ||
      mRawOutput->getBpp() != sizeof(uint16_t)) {
    ThrowRDE("Unexpected component count / data type");
  }
  mParams.validate();
}

void PanasonicV8Decompressor::decompress() const {
  const int totalStrips =
      mParams.horizontalStripCount * mParams.verticalStripCount;
#ifdef HAVE_OPENMP
  unsigned threadCount =
      std::min(totalStrips, rawspeed_get_number_of_processor_cores());
#pragma omp parallel for num_threads(threadCount)                              \
    schedule(static) default(none) shared(totalStrips)
#endif
  for (int stripIdx = 0; stripIdx < totalStrips; ++stripIdx) {
    try {
      Array1DRef<const uint8_t> strip = mParams.mStrips(stripIdx);
      Array2DRef<uint16_t> out = mParams.mOutTiles(stripIdx);

      InternalHuffDecoder decoder(mHuffmanLUT, strip);

      decompressStrip(out, decoder);
    } catch (const RawspeedException& err) {
      // Propagate the exception out of OpenMP magic.
      mRawOutput->setError(err.what());
    } catch (...) {
      // We should not get any other exception type here.
      __builtin_unreachable();
    }
  }
}

void PanasonicV8Decompressor::decompressStrip(
    const Array2DRef<uint16_t> out, InternalHuffDecoder decoder) const {
  Bayer2x2 predictedStorage = mParams.initialPrediction;
  const auto predicted = Array2DRef(predictedStorage.data(), 2, 2);

  invariant(out.height() % 2 == 0);
  invariant(out.width() % 2 == 0);

  for (int j = 0; j != 2; ++j)
    for (int i = 0; i != 2; ++i)
      predicted(i, j) = predicted(j, i);

  for (int rowGroup = 0; rowGroup < out.height() / 2; ++rowGroup) {
    const auto outRow = CroppedArray2DRef(out,
                                          /*offsetCols=*/0,
                                          /*offsetRows=*/2 * rowGroup,
                                          /*croppedWidth=*/out.width(),
                                          /*croppedHeight=*/2)
                            .getAsArray2DRef();

    // Each decoded 'row' is actually two rows of pixels in the raw image
    // because the image is encoded in rows of 2x2 CFA tiles. Likewise the
    // effective width here is 2x the strip width.
    for (int blockIdx = 0; blockIdx < out.width() / 2; ++blockIdx) {
      const auto outBlock = CroppedArray2DRef(outRow,
                                              /*offsetCols=*/2 * blockIdx,
                                              /*offsetRows=*/0,
                                              /*croppedWidth=*/2,
                                              /*croppedHeight=*/2)
                                .getAsArray2DRef();

      for (int j = 0; j != 2; ++j) {
        for (int i = 0; i != 2; ++i) {
          const int32_t diff = decoder.decodeNextDiffValue();
          const int32_t decodedValue = predicted(i, j) + diff;
          assert(decodedValue > 0);
          predicted(i, j) = uint16_t(
              std::clamp(decodedValue, 0, int32_t(mParams.gammaClipVal)));
          outBlock(i, j) = predicted(i, j);
        }
      }
    }

    const auto tmp = CroppedArray2DRef(outRow,
                                       /*offsetCols=*/0,
                                       /*offsetRows=*/0,
                                       /*croppedWidth=*/2,
                                       /*croppedHeight=*/2)
                         .getAsArray2DRef();

    // At the end of the line, reset predicted value to the first tile of the
    // prior line.
    for (int j = 0; j != 2; ++j)
      for (int i = 0; i != 2; ++i)
        predicted(i, j) = tmp(i, j);
  }
}

int32_t inline PanasonicV8Decompressor::InternalHuffDecoder::
    decodeNextDiffValue() {
  // Retrieve the difference category, which indicates magnitude of the
  // difference between the predicted and actual value.
  const auto next16 = uint16_t(mBitPump.peekBits(16));
  const auto& [bits, diffCat] = mLUT(next16);
  if (diffCat == 0 && bits == 7)
    ThrowRDE("Huffman decoding encountered an invalid value!");
  mBitPump.skipBits(bits); // Skip the bits that encoded the difference category

  if (diffCat > 0) {
    // Decode difference value. The scheme here encodes signed integers in a
    // manner similar to offset binary encoding. Here, the encoding is biased by
    // the difference category such that abs(diff) is in the range
    // [2^{diffCat-1}, 2^{diffCat}).
    const uint32_t rawDiffBits = mBitPump.getBits(diffCat);
    const uint32_t sign = rawDiffBits >> (diffCat - 1);
    const uint32_t val = rawDiffBits << 0;

    // In comments below, n = diffCat
    if (sign == 1)
      // Positive value in range [2^{n-1}, 2^{n})
      return val;
    // Negative value in interval (-2^{n}, -2^{n-1}]
    return static_cast<int32_t>(val) + static_cast<int32_t>(~0U << diffCat) + 1;
  }
  // diffBitCount of zero indicates no difference (next pixel is same as
  // predicted)
  return 0;
}

} // namespace rawspeed
