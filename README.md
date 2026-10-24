# asn1-viz

![The RFC 3280 sample certificate's UPER and APER encodings, one block per bit, visited field by field](docs/media/pkix-tour.gif)

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

## The bit strips

```sh
./build/asn1-viz datasets/pkix/certificate.layout.json      # or no argument: the same file
```

Every bit of each encoding is a block, wrapped into rows like a hex dump,
with the codecs side by side (UPER left, APER right). A block's colour is its
segment's kind: presence bits, extension bits, choice indexes, lengths,
counts, APER's alignment padding, open-type wrapping, trailing padding. Values
are coloured by ASN.1 type family. Its height is the nesting depth of its
field, a 1 bit stands taller than a 0, and a dark top means the label is
inferred rather than observed. Click a block (ray picking through
`strata_raycast_meshes`) to see its field path, type, kind, bit range, bits
and bytes. The same field then glows in every lane, joined by a link. Press
**U** to widen the selection to the parent field, **N**/**P** to step
through segments, **F** to focus, **L** to link every field of one tree
depth across the lanes, and **H** to go home.

| | |
|---|---|
| ![PKIX certificate: UPER and APER side by side, validity selected, depth-2 fields linked](docs/media/pkix-strips.png) | ![A clicked block: the third RDN value of the subject, 128 bits](docs/media/pkix-picked.png) |
| The sample certificate, UPER (8,244 bits) and APER (8,344): `validity` selected in both, every depth-2 field linked across. | A click on the subject: `rdnSequence[2][0].value`, its bits and bytes. |

![The demo message: period's length determinant picked by a click](docs/media/demo-strips.png)

The demo message (`tests/fixtures/demo.asn1`) shows every structural
element in 107 UPER and 128 APER bits. Here `period`'s length determinant
is picked: 8 bits at [11, 19).

### The canonical-UPER overlay

asn1c has no separate canonical-UPER *encoder* (`asn_encode` sends canonical
UPER through the basic encoder). The fork's canonical work is on the
*decoder* side (`ctx.uper_canonical`). So the canonical view is an imported
encoding, laid out from the decoder side, with the fork's strict canonical
checks run over it:

```sh
./build/asn1-viz-extract-pkix --value cert.der --value-codec der --codecs uper \
    --import received.per --canonical --out check.layout.json
./build/asn1-viz check.layout.json
```

![The canonical overlay: the sample certificate's UPER (left) and a copy with a non-minimal serialNumber (right), rejected by the strict check in that field](docs/media/pkix-canonical.png)

`datasets/pkix/certificate-canonical.layout.json` sets the fork's sample
certificate next to a copy whose `serialNumber` has one redundant leading
octet (made at build time by `asn1-viz-widen-integer`). The strict decoder
rejects the copy in `serialNumber`, at bit 164: X.691 11.3.6 requires an
unconstrained INTEGER's contents in the fewest octets. That field is capped
in red, and a post marks where the check fired. The lenient mode accepts the
copy, and the depth-3 links show every later field shifted by 8 bits. The
fork's own sample, imported, passes (`canonical UPER: ok`), and its
decoder-side frames match the encoder side bit for bit.

Headless, for documents and CI (EGL surfaceless; software GL works):

```sh
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
./build/asn1-viz --headless --shot out.png --pick Certificate.tbsCertificate.validity --links 2
./build/asn1-viz --headless --shot out.png --click 640 360          # what a click there picks
./build/asn1-viz --headless --tour frames/ --width 800 --height 450 && python3 tools/make_gif.py frames/ tour.gif
```

## The schema city

![The PKIX modules as a city, orbiting, with Certificate selected and what it pulls in within three hops](docs/media/pkix-city-orbit.gif)

```sh
./build/asn1-viz datasets/pkix/schema.json          # RFC 3280's two PKIX modules
./build/asn1-viz datasets/recursion/schema.json     # asn1c's 92-circular-loops-OK test schema
```

An ASN.1 specification as a city ([RFC 0024](https://github.com/Preponderous-Software/rfcs)).
Each module is a district, sized by its number of assignments and placed
next to the modules it imports from. Each type assignment is a building,
coloured by kind, its height the log of its member count. Values, value sets
and objects are flat tiles. IMPORTS are arcs between districts, drawn heavier
the more references cross them. Click a building to see its definition
(`file:line`), what it uses (cyan arcs) and what uses it (orange). Keys
**1**-**4** follow its references that many hops, which is how "what does
`Certificate` pull in" becomes visible. Types in a recursive group have a
glowing roof, and their references are drawn as red loops (**R** toggles
them).

| | |
|---|---|
| ![PKIX city, Certificate selected, three hops](docs/media/pkix-city.png) | ![The recursion test schema: 12 of 13 types in cycles](docs/media/recursion-city.png) |
| PKIX: 2 modules, 126 types, 101 type-to-type references (6 across the IMPORTS arc). `Certificate` reaches 12 types within three hops. | `92-circular-loops-OK.asn1`: 13 types, 30 references, 3 recursive groups. `Everything` selected. |

The model comes from asn1c's own parser and fixer, not from its printed
output. `asn1-viz-schema` links `libasn1parser`/`libasn1fix` (built with the
fork's `config.h`), runs `asn1f_process`, resolves every reference with
`asn1f_lookup_symbol_ex` in the referring module's namespace (as the compiler
does), and finds recursion with Tarjan's SCCs. It writes `asn1-schema/1`
JSON. When a layout is opened, a `schema.json` beside it fills in the strip
view's `file:line`.

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
| `pkix` (city) | as above | | `datasets/pkix/schema.json` |
| `recursion` (city) | the fork's `tests/tests-asn1c-compiler/92-circular-loops-OK.asn1` | | `datasets/recursion/schema.json` |

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
./build/asn1-viz               # a window: drag to orbit, right-drag to pan, wheel to zoom, click to pick
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
