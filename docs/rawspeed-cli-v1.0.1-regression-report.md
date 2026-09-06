rawspeed-cli v1.0.1 regression report
=====================================

Symptom
-------

rawspeed-cli v1.0.1 fails on ARW/CR2/PEF inputs with::

  rawspeed: unsupported CFA pattern
  exit 1

v1.0.0 decoded the same files.

Root cause
----------

``main.cpp`` only calls ``decoder->decodeRaw()`` and never
``decoder->decodeMetaData()``. For ARW/CR2/PEF the CFA pattern is set
in the metadata pass, not in the raw pass:

- ``ArwDecoder::decodeMetaDataInternal`` (ArwDecoder.cpp:501) calls
  ``mRaw->cfa.setCFA(2x2, R, G, G, B)`` before consulting the camera
  database; the same holds for ``Cr2Decoder`` and ``PefDecoder``.
- ``render()`` then reads ``raw->cfa`` with its default size (0, 0)
  and fails at the ``cfaSize != 2x2`` check.

The old v1.0.0 CLI only worked because it unconditionally called
``raw->cfa.setCFA(2x2, R,G,G,B)`` in ``render()``. That call looked
like dead code and was removed during the clang-tidy cleanup - it was
load-bearing.

Fix (v1.0.2)
------------

1. After ``decodeRaw()``, call ``decoder->decodeMetaData(&meta)``.
   ``cameras.xml`` is resolved like ``rawspeed-identify`` does
   (``RS_CAMERAS_XML_PATH``, ``<bindir>/../share/darktable/rawspeed/
   cameras.xml``, then ``RAWSPEED_SOURCE_DIR/data/cameras.xml`` for
   standalone builds). If it cannot be found or parsed, an empty
   ``CameraMetaData{}`` is used: ``ArwDecoder`` (and Cr2/Pef) set the
   RGGB CFA before consulting the database, and with
   ``failOnUnknown = false`` an unknown camera only logs a warning and
   returns early, so decoding works without ``cameras.xml``. A failing
   metadata pass is logged and skipped rather than aborting the
   decode.
2. ``render()`` is hardened: if the reported CFA size is smaller than
   2x2, it defaults to RGGB instead of failing. Larger CFA patterns
   (e.g. X-Trans) still fail as unsupported. The existing 4-Bayer
   pattern mapping is kept - it is correct once the CFA is real.
3. The publish workflow's smoke test now decodes a real image (a
   synthetic 8x8 uncompressed CFA DNG generated in the runner) and
   verifies the PPM output, instead of only checking the usage path
   which cannot catch this class of regression.

Verification
------------

- synthetic 8x8 CFA DNG decodes: exit 0, ``P6 8 8 65535``, 397 bytes;
  8-bit variant: 203 bytes
- usage path still exits 2 with usage text
- clang-format 18.1.8 idempotent, clang -Weverything -Werror clean,
  clang-tidy (CI check set) clean, MSVC Release build green

AI note
-------

This report and the fix were prepared with AI assistance (opencode),
per AGENTS.md disclosure rules.
