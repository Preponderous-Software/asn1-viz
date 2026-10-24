# asn1-viz

3D views of ASN.1 encodings and schemas, drawn on
[Strata](https://github.com/Preponderous-Software/strata) from what
[asn1c](https://github.com/dmccoystephenson/asn1c) actually does.

- **Bit-layout visualizer** ([Preponderous RFC 0023](https://github.com/Preponderous-Software/rfcs)):
  the UPER and APER encodings of one value as side-by-side 3D strips, every
  segment (value, preamble bit, length determinant, alignment padding, ...)
  a block you can pick to see its field path and its bits.
- **Schema city** (RFC 0024): an ASN.1 specification as a city, one district
  per module and one building per type, with references and recursion drawn
  as lines.

Work in progress: built milestone by milestone as the RFCs lay out.

## Building

Needs CMake 3.24+, a C++17 compiler, and (for the extractor, Linux only)
autotools, bison and flex to build the pinned asn1c fork.

```sh
cmake -S . -B build            # fetches asn1c at asn1c.ref and Strata at strata.ref
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/asn1-viz               # a window; drag to orbit, right-drag to pan, wheel to zoom, click to pick
```

Headless (no display; EGL surfaceless, software GL works):

```sh
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./build/asn1-viz --headless --shot out.png
```

On Windows (MSVC) the viewer and its tests build; the extractor does not
(it needs the fork's autotools build and GNU ld's `--wrap`), so the viewer
reads the prebuilt files in `datasets/`.

### Pins

| File | What | Bump |
|---|---|---|
| [`asn1c.ref`](asn1c.ref) | the `dmccoystephenson/asn1c` commit (on `vlm_master`) used to generate code and build `libasn1*` | a deliberate PR; CI reruns every dataset's self-test |
| [`strata.ref`](strata.ref) | the Strata commit (v0.4.0, ABI 1.8) | a deliberate PR |

Local checkouts replace either download:
`-DFETCHCONTENT_SOURCE_DIR_ASN1C=<path>` (an unconfigured tree) and
`-DFETCHCONTENT_SOURCE_DIR_STRATA=<path>`.

## Licence

MIT (see [LICENSE](LICENSE)). asn1c is BSD-2-Clause; see
[THIRD-PARTY.md](THIRD-PARTY.md).
