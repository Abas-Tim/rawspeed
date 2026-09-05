#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "RawSpeed-API.h"

using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

using rawspeed::CFAColor;

struct DemosaicJob {
  rawspeed::Array2DRef<uint16_t> mosaic;
  uint16_t* rgb;
  uint32_t rgbWidth;
  uint32_t rgbHeight;
  int patternCode;
  float wb[3];

  explicit DemosaicJob(rawspeed::Array2DRef<uint16_t> m)
      : mosaic(m), rgb(nullptr), rgbWidth(0), rgbHeight(0), patternCode(0),
        wb{1, 1, 1} {}
};

static int cfaIndexOf(int patternCode, uint32_t row, uint32_t col) {
  static constexpr int code[4][4] = {
      {1, 0, 2, 1},
      {0, 1, 1, 2},
      {2, 1, 1, 0},
      {1, 2, 0, 1},
  };
  return code[patternCode][((row & 1) << 1) | (col & 1)];
}

static uint16_t clampToU16(float value) {
  if (value <= 0.0F)
    return 0;
  if (value >= 65535.0F)
    return 65535;
  return static_cast<uint16_t>(value);
}

static void demosaicBand(const DemosaicJob& j, uint32_t yStart,
                         uint32_t yEnd) {
  for (uint32_t y = yStart; y < yEnd && y < j.rgbHeight; ++y) {
    for (uint32_t x = 0; x < j.rgbWidth; ++x) {
      float acc[3] = {0, 0, 0};
      int counts[3] = {0, 0, 0};
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
          const float w = (dx == 0 && dy == 0)
                              ? 4.0F
                              : (std::abs(dx) + std::abs(dy) == 1) ? 1.0F
                                                                    : 0.25F;
          acc[c] += v * w;
          counts[c]++;
        }
      }

      float r = counts[0] ? acc[0] / (counts[0] * 2.0F)
                          : acc[1] / std::max(1, counts[1]);
      float g = counts[1] ? acc[1] / (counts[1] * 2.0F) : 0;
      float b = counts[2] ? acc[2] / (counts[2] * 2.0F)
                          : acc[1] / std::max(1, counts[1]);
      if (counts[1] == 0)
        g = counts[0] ? r : b;

      r *= j.wb[0];
      g *= j.wb[1];
      b *= j.wb[2];

      uint16_t* dst = j.rgb + (static_cast<size_t>(y) * j.rgbWidth + x) * 3;
      dst[0] = clampToU16(r);
      dst[1] = clampToU16(g);
      dst[2] = clampToU16(b);
    }
  }
}

static bool render(rawspeed::RawImage raw, std::vector<uint16_t>& rgbOut,
                   uint32_t& outW, uint32_t& outH) {
  if (!raw->isCFA || raw->getDataType() != rawspeed::RawImageType::UINT16 ||
      raw->getCpp() != 1) {
    std::fprintf(stderr, "rawspeed: only 16-bit Bayer CFA images are supported\n");
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

  double sum[3] = {0, 0, 0};
  uint64_t cnt[3] = {0, 0, 0};
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const int c = cfaIndexOf(pattern, y, x);
      const float v = static_cast<float>(mosaic(y, x));
      sum[c] += v;
      cnt[c]++;
    }
  }

  float wb[3] = {1, 1, 1};
  for (int c = 0; c < 3; ++c) {
    if (cnt[c] > 0 && cnt[1] > 0 && sum[c] > 0.0 && sum[1] > 0.0)
      wb[c] = static_cast<float>((sum[1] / cnt[1]) / (sum[c] / cnt[c]));
  }
  wb[1] = 1.0F;

  rgbOut.assign(static_cast<size_t>(w) * h * 3, 0);
  DemosaicJob job(mosaic);
  job.rgb = rgbOut.data();
  job.rgbWidth = w;
  job.rgbHeight = h;
  job.patternCode = pattern;
  job.wb[0] = wb[0];
  job.wb[1] = 1.0F;
  job.wb[2] = wb[2];

  const unsigned availableThreads = std::thread::hardware_concurrency();
  const unsigned nThreads = std::min(
      48u, std::max(1u, std::min(availableThreads == 0 ? 1u
                                                        : availableThreads,
                                  h)));
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

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: rawspeed-cli <in.raw> <out.ppm>\n");
    return 2;
  }

  try {
    rawspeed::FileReader reader(argv[1]);
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

    FILE* f = std::fopen(argv[2], "wb");
    if (!f) {
      std::fprintf(stderr, "rawspeed: cannot write output\n");
      return 1;
    }
    const bool eightBit = argc > 3 && argv[3][0] == '8';
    if (eightBit)
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    else
      std::fprintf(f, "P6\n%u %u\n65535\n", w, h);

    std::vector<uint8_t> row(static_cast<size_t>(w) *
                             (eightBit ? 3u : 6u));
    for (uint32_t y = 0; y < h; ++y) {
      size_t offset = 0;
      const uint16_t* src = rgb.data() + static_cast<size_t>(y) * w * 3;
      for (uint32_t x = 0; x < w; ++x) {
        const uint16_t r = src[x * 3 + 0];
        const uint16_t g = src[x * 3 + 1];
        const uint16_t b = src[x * 3 + 2];
        if (eightBit) {
          row[offset++] = static_cast<uint8_t>(r >> 8);
          row[offset++] = static_cast<uint8_t>(g >> 8);
          row[offset++] = static_cast<uint8_t>(b >> 8);
        } else {
          row[offset++] = static_cast<uint8_t>(r >> 8);
          row[offset++] = static_cast<uint8_t>(r & 0xFF);
          row[offset++] = static_cast<uint8_t>(g >> 8);
          row[offset++] = static_cast<uint8_t>(g & 0xFF);
          row[offset++] = static_cast<uint8_t>(b >> 8);
          row[offset++] = static_cast<uint8_t>(b & 0xFF);
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
