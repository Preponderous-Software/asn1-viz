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

## Licence

MIT (see [LICENSE](LICENSE)). asn1c is BSD-2-Clause; see
[THIRD-PARTY.md](THIRD-PARTY.md).
