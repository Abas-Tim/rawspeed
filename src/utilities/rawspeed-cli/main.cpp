#include "RawSpeed-API.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using rawspeed::CFAColor;

struct DemosaicJob {
  rawspeed::Array2DRef<uint16_t> mosaic;
  rawspeed::Array1DRef<uint16_t> rgb;
  uint32_t rgbWidth;
  uint32_t rgbHeight;
  int patternCode;
  std::array<float, 3> wb;

  DemosaicJob(rawspeed::Array2DRef<uint16_t> m,
              rawspeed::Array1DRef<uint16_t> r)
      : mosaic(m), rgb(r), rgbWidth(0), rgbHeight(0), patternCode(0),
        wb{1.0F, 1.0F, 1.0F} {}
};

static int cfaIndexOf(int patternCode, uint32_t row, uint32_t col) {
  static constexpr std::array<std::array<int, 4>, 4> code = {{
      {1, 0, 2, 1},
      {0, 1, 1, 2},
      {2, 1, 1, 0},
      {1, 2, 0, 1},
  }};
  return code.at(static_cast<size_t>(patternCode))
      .at(((row & 1) << 1) | (col & 1));
}

static uint16_t clampToU16(float value) {
  if (value <= 0.0F)
    return 0;
  if (value >= 65535.0F)
    return 65535;
  return static_cast<uint16_t>(value);
}

static void demosaicBand(const DemosaicJob& j, uint32_t yStart, uint32_t yEnd) {
  for (uint32_t y = yStart; y < yEnd && y < j.rgbHeight; ++y) {
    for (uint32_t x = 0; x < j.rgbWidth; ++x) {
      std::array<float, 3> acc = {0.0F, 0.0F, 0.0F};
      std::array<int, 3> counts = {0, 0, 0};
      for (int dy = -2; dy <= 2; ++dy) {
        const int yy = static_cast<int>(y) + dy;
        if (yy < 0 || yy >= j.mosaic.height())
          continue;
        for (int dx = -2; dx <= 2; ++dx) {
          const int xx = static_cast<int>(x) + dx;
          if (xx < 0 || xx >= j.mosaic.width())
            continue;
          const int c = cfaIndexOf(j.patternCode, static_cast<uint32_t>(yy),
                                   static_cast<uint32_t>(xx));
          const float v = static_cast<float>(j.mosaic(yy, xx));
          const int dist = std::abs(dx) + std::abs(dy);
          const float w = dist == 0 ? 4.0F : dist == 1 ? 1.0F : 0.25F;
          acc.at(c) += v * w;
          counts.at(c)++;
        }
      }

      const int cnt0 = counts.at(0);
      const int cnt1 = counts.at(1);
      const int cnt2 = counts.at(2);
      const float fallback = acc.at(1) / static_cast<float>(std::max(1, cnt1));
      float r =
          cnt0 != 0 ? acc.at(0) / (static_cast<float>(cnt0) * 2.0F) : fallback;
      float g =
          cnt1 != 0 ? acc.at(1) / (static_cast<float>(cnt1) * 2.0F) : 0.0F;
      float b =
          cnt2 != 0 ? acc.at(2) / (static_cast<float>(cnt2) * 2.0F) : fallback;
      if (cnt1 == 0)
        g = cnt0 != 0 ? r : b;

      r *= j.wb.at(0);
      g *= j.wb.at(1);
      b *= j.wb.at(2);

      const int pix = static_cast<int>(static_cast<size_t>(y) * j.rgbWidth + x);
      j.rgb(pix * 3 + 0) = clampToU16(r);
      j.rgb(pix * 3 + 1) = clampToU16(g);
      j.rgb(pix * 3 + 2) = clampToU16(b);
    }
  }
}

static bool render(rawspeed::RawImage raw, std::vector<uint16_t>& rgbOut,
                   uint32_t& outW, uint32_t& outH) {
  if (!raw->isCFA || raw->getDataType() != rawspeed::RawImageType::UINT16 ||
      raw->getCpp() != 1) {
    std::fprintf(stderr,
                 "rawspeed: only 16-bit Bayer CFA images are supported\n");
    return false;
  }

  const auto cfaSize = raw->cfa.getSize();
  if (cfaSize.x != 2 || cfaSize.y != 2) {
    std::fprintf(stderr, "rawspeed: unsupported CFA pattern\n");
    return false;
  }

  const auto c00 = raw->cfa.getColorAt(0, 0);
  const auto c01 = raw->cfa.getColorAt(1, 0);
  const auto c10 = raw->cfa.getColorAt(0, 1);
  const auto c11 = raw->cfa.getColorAt(1, 1);

  int pattern = -1;
  if (c00 == CFAColor::GREEN && c11 == CFAColor::GREEN &&
      c01 == CFAColor::RED && c10 == CFAColor::BLUE)
    pattern = 0;
  else if (c00 == CFAColor::GREEN && c11 == CFAColor::GREEN &&
           c01 == CFAColor::BLUE && c10 == CFAColor::RED)
    pattern = 3;
  else if (c00 == CFAColor::RED && c11 == CFAColor::BLUE &&
           c01 == CFAColor::GREEN && c10 == CFAColor::GREEN)
    pattern = 1;
  else if (c00 == CFAColor::BLUE && c11 == CFAColor::RED &&
           c01 == CFAColor::GREEN && c10 == CFAColor::GREEN)
    pattern = 2;

  if (pattern < 0) {
    std::fprintf(stderr, "rawspeed: unsupported CFA pattern\n");
    return false;
  }

  const auto mosaic = raw->getU16DataAsCroppedArray2DRef().getAsArray2DRef();
  const uint32_t w = static_cast<uint32_t>(mosaic.width());
  const uint32_t h = static_cast<uint32_t>(mosaic.height());
  if (w == 0 || h == 0) {
    std::fprintf(stderr, "rawspeed: decoded image is empty\n");
    return false;
  }

  std::array<double, 3> sum = {0.0, 0.0, 0.0};
  std::array<uint64_t, 3> cnt = {0, 0, 0};
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const int c = cfaIndexOf(pattern, y, x);
      sum.at(c) += static_cast<double>(mosaic(y, x));
      cnt.at(c)++;
    }
  }

  std::array<float, 3> wb = {1.0F, 1.0F, 1.0F};
  for (int c = 0; c < 3; ++c) {
    if (cnt.at(c) > 0 && cnt.at(1) > 0 && sum.at(c) > 0.0 && sum.at(1) > 0.0)
      wb.at(c) =
          static_cast<float>(sum.at(1) / static_cast<double>(cnt.at(1)) /
                             (sum.at(c) / static_cast<double>(cnt.at(c))));
  }
  wb.at(1) = 1.0F;

  rgbOut.assign(static_cast<size_t>(w) * h * 3, 0);
  DemosaicJob job(
      mosaic,
      rawspeed::Array1DRef<uint16_t>(
          rgbOut.data(), static_cast<int>(static_cast<size_t>(w) * h * 3)));
  job.rgbWidth = w;
  job.rgbHeight = h;
  job.patternCode = pattern;
  job.wb.at(0) = wb.at(0);
  job.wb.at(1) = 1.0F;
  job.wb.at(2) = wb.at(2);

  const unsigned availableThreads = std::thread::hardware_concurrency();
  const unsigned nThreads = std::min(
      48U,
      std::max(1U, std::min(availableThreads == 0 ? 1U : availableThreads, h)));
  const uint32_t band = (h + nThreads - 1) / nThreads;
  std::vector<std::thread> threads;
  threads.reserve(nThreads);
  for (unsigned t = 0; t < nThreads; ++t)
    threads.emplace_back(demosaicBand, std::cref(job), t * band,
                         (t + 1) * band);
  for (auto& thread : threads)
    thread.join();

  outW = w;
  outH = h;
  return true;
}

int main(int argc_, char** argv_) {
  const auto argv = rawspeed::Array1DRef(argv_, argc_);
  if (argv.size() < 3) {
    std::fprintf(stderr, "usage: rawspeed-cli <in.raw> <out.ppm>\n");
    return 2;
  }

  try {
    rawspeed::FileReader reader(argv(1));
    auto [storage, buffer] = reader.readFile();
    (void)storage;
    rawspeed::RawParser parser(std::move(buffer));
    auto decoder = parser.getDecoder();
    decoder->failOnUnknown = false;

    rawspeed::RawImage raw = decoder->decodeRaw();

    std::vector<uint16_t> rgb;
    uint32_t w = 0;
    uint32_t h = 0;
    if (!render(raw, rgb, w, h))
      return 1;

    FILE* f = std::fopen(argv(2), "wb");
    if (!f) {
      std::fprintf(stderr, "rawspeed: cannot write output\n");
      return 1;
    }
    const bool eightBit =
        argv.size() > 3 && std::string_view(argv(3)).starts_with('8');
    if (eightBit)
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    else
      std::fprintf(f, "P6\n%u %u\n65535\n", w, h);

    std::vector<uint8_t> row(static_cast<size_t>(w) * (eightBit ? 3U : 6U));
    for (uint32_t y = 0; y < h; ++y) {
      size_t offset = 0;
      const size_t rowBase = static_cast<size_t>(y) * w * 3;
      for (uint32_t x = 0; x < w; ++x) {
        const uint16_t r = rgb.at(rowBase + static_cast<size_t>(x) * 3U);
        const uint16_t g = rgb.at(rowBase + static_cast<size_t>(x) * 3U + 1);
        const uint16_t b = rgb.at(rowBase + static_cast<size_t>(x) * 3U + 2);
        if (eightBit) {
          row.at(offset++) = static_cast<uint8_t>(r >> 8);
          row.at(offset++) = static_cast<uint8_t>(g >> 8);
          row.at(offset++) = static_cast<uint8_t>(b >> 8);
        } else {
          row.at(offset++) = static_cast<uint8_t>(r >> 8);
          row.at(offset++) = static_cast<uint8_t>(r & 0xFF);
          row.at(offset++) = static_cast<uint8_t>(g >> 8);
          row.at(offset++) = static_cast<uint8_t>(g & 0xFF);
          row.at(offset++) = static_cast<uint8_t>(b >> 8);
          row.at(offset++) = static_cast<uint8_t>(b & 0xFF);
        }
      }
      if (std::fwrite(row.data(), 1, offset, f) != offset) {
        std::fclose(f);
        std::fprintf(stderr, "rawspeed: cannot write output\n");
        return 1;
      }
    }
    if (std::fclose(f) != 0) {
      std::fprintf(stderr, "rawspeed: cannot write output\n");
      return 1;
    }
    std::fprintf(stderr, "rawspeed: done %ux%u\n", w, h);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rawspeed: error: %s\n", e.what());
    return 1;
  }
}
