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

## The extractor

`asn1-viz-extract-<dataset>` decodes a value with an asn1c-generated codec,
encodes it in UPER and APER with a tracer in the loop, and writes an
`asn1-layout/1` JSON file: every encode call as a frame (field path, ASN.1
type, bit span) and every bit of the stream in exactly one segment (value,
presence bit, extension bit, length, count, alignment, choice index, open-type
length or padding, trailing padding). asn1c is used unmodified:

- **Tier 1**: a shim is written into each type's PER op table, so every
  type-level encode is seen with its start and end bit.
- **Tier 2**: the bit writers and the length, normally-small and alignment
  helpers are wrapped at link time (`-Wl,--wrap=...`), so every write is seen
  with its exact position and the helpers label theirs.
- **Tier 3**: bits a constructed type writes itself (a SEQUENCE's extension
  and presence bits, a CHOICE index, a leaf's extension bit, a constrained
  string's length) are labelled from the type descriptor in X.691 order and
  marked `"source": "inferred"`.

Open types and canonically sorted `SET OF` elements, which asn1c encodes into
separate buffers first, are relocated to where their bits land. Every run
self-tests: the segments tile the stream with no gap or overlap, the traced
bytes equal an untraced encoding, and the root span equals the encoded
length. If any check fails, nothing is written.

```sh
./build/asn1-viz-extract-pkix --value sample-Certificate-1.der --value-codec der --out cert.layout.json
python3 tools/layout_png.py cert.layout.json cert.png      # a flat 2D look (Pillow)
```

## Datasets

| Dataset | Schema | Value | In this repository |
|---|---|---|---|
| `pkix` | RFC 3280 `PKIX1Explicit88` + `PKIX1Implicit88`, extracted at build time from the pinned fork's `examples/rfc3280.txt` with its `crfc2asn1.pl` | the fork's `sample-Certificate-1.der` | `datasets/pkix/certificate.layout.json` (UPER 1,031 octets, APER 1,043; both byte-identical to the fork's own `.per`/`.aper` samples) |
| `demo` | `tests/fixtures/demo.asn1` (written for this repository) | `tests/fixtures/demo-msg.xer` | `datasets/demo/msg.layout.json` |

The committed layouts are what the pinned fork produces: CI regenerates them
and fails if they differ (`cmake --build build --target update-datasets`
refreshes them after a deliberate pin bump).

Not shipped, as follow-ups: **LTE RRC** (3GPP TS 36.331; not in the fork, and
3GPP's redistribution terms are unverified), **ETSI ITS CAM/DENM** (terms of
the ASN.1 on forge.etsi.org unverified), and the fork's **UMTS RRC 7.1.0**,
whose header forbids redistribution. Layouts embed field names and values,
so none of these is committed or published until its terms are confirmed.

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
