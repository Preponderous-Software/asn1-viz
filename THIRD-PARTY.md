# Third-party components

asn1-viz itself is MIT-licensed (see [LICENSE](LICENSE)). It builds against,
and its outputs derive from, the components below. None of them is vendored
in this repository: each is fetched at a pinned commit at build time.

## asn1c (BSD-2-Clause)

- Source: <https://github.com/dmccoystephenson/asn1c>, a fork of
  <https://github.com/mouse07410/asn1c>, pinned in [`asn1c.ref`](asn1c.ref).
- Licence: BSD-2-Clause. Copyright (c) 2003-2017 Lev Walkin and contributors;
  copyright (c) 2022-2026 Mouse and contributors.
- Used for: the `asn1c` compiler (generates the codec the extractor traces),
  its runtime skeletons (compiled into the extractor), and `libasn1parser`,
  `libasn1fix` and `libasn1common` (linked into the schema walker).
- Code that asn1c generates carries asn1c's notice. Extractor binaries
  include the skeletons and are therefore covered by BSD-2-Clause as well as
  MIT.

### Data derived from the fork

- `datasets/pkix/*.json`: layouts and the schema walk of RFC 3280's
  `PKIX1Explicit88`/`PKIX1Implicit88` modules (extracted at build time from
  the fork's `examples/rfc3280.txt`) and of the fork's sample certificate
  (`examples/sample.source.PKIX1/sample-Certificate-1.der`). RFC 3280's Full
  Copyright Statement permits derivative works that assist in its
  implementation, with the notice kept: Copyright (C) The Internet Society
  (2002). All Rights Reserved.
- `datasets/recursion/schema.json`: the schema walk of the fork's test schema
  `tests/tests-asn1c-compiler/92-circular-loops-OK.asn1` (BSD-2-Clause, as
  above).

## Strata (MIT)

- Source: <https://github.com/Preponderous-Software/strata>, pinned in
  [`strata.ref`](strata.ref) (the v0.4.0 release).
- Licence: MIT. Copyright (c) 2026 Daniel McCoy Stephenson.
- Used for: rendering, picking and screenshots. Strata in turn fetches SDL3
  (zlib licence) and glad (MIT / public domain for generated code).
