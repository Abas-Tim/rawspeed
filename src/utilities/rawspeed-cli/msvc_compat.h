#pragma once

#if defined(_MSC_VER) && !defined(__clang__)

#ifndef __attribute__
#define __attribute__(x)
#endif

#ifndef __PRETTY_FUNCTION__
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#ifndef __builtin_unreachable
#define __builtin_unreachable() __assume(false)
#endif

#include <cstdint>
#include <limits>
#include <type_traits>

namespace rawspeed_msvc {

template <typename T> inline bool ovf_add(T a, T b, T* r) {
  static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
  const long long sum = static_cast<long long>(a) + static_cast<long long>(b);
  *r = static_cast<T>(sum);
  return sum > static_cast<long long>(std::numeric_limits<T>::max()) ||
         sum < static_cast<long long>(std::numeric_limits<T>::min());
}

template <typename T> inline bool ovf_mul(T a, T b, T* r) {
  static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
  if constexpr (std::is_signed_v<T>) {
    const long long product =
        static_cast<long long>(a) * static_cast<long long>(b);
    *r = static_cast<T>(product);
    return product > static_cast<long long>(std::numeric_limits<T>::max()) ||
           product < static_cast<long long>(std::numeric_limits<T>::min());
  } else {
    const unsigned long long product =
        static_cast<unsigned long long>(a) * static_cast<unsigned long long>(b);
    *r = static_cast<T>(product);
    return product >
           static_cast<unsigned long long>(std::numeric_limits<T>::max());
  }
}

} // namespace rawspeed_msvc

#define __builtin_sadd_overflow(a, b, c) rawspeed_msvc::ovf_add((a), (b), (c))
#define __builtin_mul_overflow(a, b, c) rawspeed_msvc::ovf_mul((a), (b), (c))

#endif
