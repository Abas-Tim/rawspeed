#!/bin/bash -eu
# Copyright 2017 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

set -ex

apt-get install -y ninja-build
export CMAKE_GENERATOR=Ninja

RAWSPEED_SOURCE="$SRC/librawspeed/"
RAWSPEED_BUILD="$WORK/rawspeed"

ln -f -s /usr/local/bin/lld /usr/bin/ld

CFLAGS="$CFLAGS -flto=thin"
CXXFLAGS="$CXXFLAGS -flto=thin"

CXXFLAGS="$CXXFLAGS -fforce-emit-vtables"
# CXXFLAGS="$CXXFLAGS -fwhole-program-vtables" # DOES NOT WORK WITH SANCOV!
CXXFLAGS="$CXXFLAGS -fstrict-vtable-pointers"

THINLTO_CACHE="$WORK/thinlto-cache"
LDFLAGS="${LDFLAGS:-} -Wl,--thinlto-cache-dir=\"$THINLTO_CACHE\""

cd "$SRC"

OSSFUZZ_LLVM_VER=$(clang -dumpversion)
OSSFUZZ_LLVM_VER_MAJOR=(${OSSFUZZ_LLVM_VER//./ })
OSSFUZZ_LLVM_VER_MAJOR=${OSSFUZZ_LLVM_VER_MAJOR[0]}

wget -q https://github.com/llvm/llvm-project/releases/download/llvmorg-$OSSFUZZ_LLVM_VER/llvm-project-$OSSFUZZ_LLVM_VER.src.tar.xz
tar -xf llvm-project-$OSSFUZZ_LLVM_VER.src.tar.xz llvm-project-$OSSFUZZ_LLVM_VER.src/{runtimes,cmake,llvm/cmake,libcxx,libcxxabi,compiler-rt}/
COMPILERRT_LLVM_SOURCE="$SRC/llvm-project-$OSSFUZZ_LLVM_VER.src"

COMPILERRT_BUILD="$WORK/llvm-project-$OSSFUZZ_LLVM_VER.compiler-rt.build"
cmake -S "$COMPILERRT_LLVM_SOURCE/runtimes/" -B "$COMPILERRT_BUILD" \
      -DCMAKE_C_FLAGS="$CFLAGS -fno-sanitize=all" \
      -DCMAKE_CXX_FLAGS="$CXXFLAGS -fno-sanitize=all" \
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
      -DCMAKE_C_VISIBILITY_PRESET=hidden \
      -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
      -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLIBCXX_INCLUDE_TESTS=OFF \
      -DLIBCXXABI_INCLUDE_TESTS=OFF \
      -DCOMPILER_RT_INCLUDE_TESTS=OFF \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;compiler-rt" \
      -DLIBCXX_ENABLE_SHARED=OFF \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
      -DLIBCXXABI_ENABLE_SHARED=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
      -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
      -DCOMPILER_RT_USE_LLVM_UNWINDER=OFF \
      -DCOMPILER_RT_CXX_LIBRARY=libcxx \
      -DCOMPILER_RT_STATIC_CXX_LIBRARY=ON \
      -DSANITIZER_USE_STATIC_CXX_ABI=ON \
      -DSANITIZER_CXX_ABI_LIBNAME=libc++ \
      -DSANITIZER_CXX_ABI_INTREE=ON \
      -DCOMPILER_RT_SANITIZERS_TO_BUILD="asan;msan" \
      -DCOMPILER_RT_BUILD_PROFILE=OFF \
      -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
      -DCOMPILER_RT_BUILD_XRAY=OFF \
      -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
      -DCOMPILER_RT_BUILD_MEMPROF=OFF \
      -DCOMPILER_RT_BUILD_ORC=OFF
cmake --build "$COMPILERRT_BUILD" -- -j$(nproc) compiler-rt

TMPPDIR=$(mktemp -d)
mv /usr/local/lib/clang/$OSSFUZZ_LLVM_VER_MAJOR/lib/x86_64-unknown-linux-gnu/libclang_rt.fuzzer* $TMPPDIR/

rm -rf /usr/*/lib/clang/*/include/sanitizer
rm -rf /usr/*/lib/clang/*/lib/*

ln -s $COMPILERRT_BUILD/compiler-rt/lib/linux /usr/local/lib/clang/$OSSFUZZ_LLVM_VER_MAJOR/lib/linux
ln -s $COMPILERRT_BUILD/compiler-rt/lib/linux /usr/local/lib/clang/$OSSFUZZ_LLVM_VER_MAJOR/lib/x86_64-unknown-linux-gnu

mv $TMPPDIR/* $COMPILERRT_BUILD/compiler-rt/lib/linux

rm -rf $COMPILERRT_BUILD/compiler-rt/lib/linux/*.so

CXXFLAGS="$CXXFLAGS -isystem $COMPILERRT_BUILD/compiler-rt/include"

LIBCXX_LLVM_VER="19.1.7"

wget -q https://github.com/llvm/llvm-project/releases/download/llvmorg-$LIBCXX_LLVM_VER/llvm-project-$LIBCXX_LLVM_VER.src.tar.xz
tar -xf llvm-project-$LIBCXX_LLVM_VER.src.tar.xz llvm-project-$LIBCXX_LLVM_VER.src/{runtimes,cmake,llvm/cmake,libcxx,libcxxabi}/
LIBCXX_LLVM_SOURCE="$SRC/llvm-project-$LIBCXX_LLVM_VER.src"

LIBCXX_BUILD="$WORK/llvm-project-$LIBCXX_LLVM_VER.libcxx.build"
cmake -S "$LIBCXX_LLVM_SOURCE/runtimes/" -B "$LIBCXX_BUILD" \
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
      -DCMAKE_C_VISIBILITY_PRESET=hidden \
      -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
      -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
      -DLIBCXX_ENABLE_SHARED=OFF \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
      -DLIBCXXABI_ENABLE_SHARED=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
      -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
      -DLIBCXXABI_ADDITIONAL_COMPILE_FLAGS="-fno-sanitize=vptr"
cmake --build "$LIBCXX_BUILD" -- -j$(nproc) cxx cxxabi

CXXFLAGS="$CXXFLAGS -nostdinc++ -nostdlib++ -isystem $LIBCXX_BUILD/include -isystem $LIBCXX_BUILD/include/c++/v1 -L$LIBCXX_BUILD/lib -lc++ -lc++abi"

LIBOMP_LLVM_VER="20.1.5"

wget -q https://github.com/llvm/llvm-project/releases/download/llvmorg-$LIBOMP_LLVM_VER/llvm-project-$LIBOMP_LLVM_VER.src.tar.xz
tar -xf llvm-project-$LIBOMP_LLVM_VER.src.tar.xz llvm-project-$LIBOMP_LLVM_VER.src/{runtimes,cmake,llvm/cmake,openmp}/
OPENMP_LLVM_SOURCE="$SRC/llvm-project-$LIBOMP_LLVM_VER.src"

patch $OPENMP_LLVM_SOURCE/openmp/runtime/src/kmp.h $RAWSPEED_SOURCE/.ci/openmp.patch

OPENMP_BUILD="$WORK/llvm-project-$LIBOMP_LLVM_VER.omp.build"
cmake -S "$OPENMP_LLVM_SOURCE/openmp/" -B "$OPENMP_BUILD" \
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
      -DCMAKE_C_VISIBILITY_PRESET=hidden \
      -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
      -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLIBOMP_ENABLE_SHARED=OFF \
      -DOPENMP_ENABLE_LIBOMPTARGET=OFF \
      -DLIBOMP_CXXFLAGS="-fno-sanitize=undefined,integer"
cmake --build "$OPENMP_BUILD" -- -j$(nproc) omp

CXXFLAGS="$CXXFLAGS -isystem $OPENMP_BUILD/runtime/src -L$OPENMP_BUILD/runtime/src"

if [[ $SANITIZER = *undefined* ]]; then
  CFLAGS="$CFLAGS -fsanitize=unsigned-integer-overflow -fno-sanitize-recover=unsigned-integer-overflow"
  CXXFLAGS="$CXXFLAGS -fsanitize=unsigned-integer-overflow -fno-sanitize-recover=unsigned-integer-overflow"
fi

cmake -S "$RAWSPEED_SOURCE" -B "$RAWSPEED_BUILD" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DBINARY_PACKAGE_BUILD=ON -DWITH_OPENMP=ON \
  -DWITH_PUGIXML=OFF -DUSE_XMLLINT=OFF -DWITH_JPEG=OFF -DWITH_ZLIB=OFF \
  -DBUILD_TESTING=OFF -DBUILD_TOOLS=OFF -DBUILD_BENCHMARKING=OFF \
  -DCMAKE_BUILD_TYPE=FUZZ -DBUILD_FUZZERS=ON \
  -DLIB_FUZZING_ENGINE:STRING="$LIB_FUZZING_ENGINE" \
  -DCMAKE_INSTALL_PREFIX:PATH="$OUT" -DCMAKE_INSTALL_BINDIR:PATH="$OUT"

cmake --build "$RAWSPEED_BUILD" -- -j$(nproc) all && cmake --build "$RAWSPEED_BUILD" -- -j$(nproc) install

du -hcs "$SRC"/* \
        "$WORK"/* \
        "$OUT"
