# Design notes

Things about this format that look like mistakes and are not. Read before
"fixing" one of them.

## TILE_PIXEL_DATA grows backwards, on purpose

Every other structure in the IFE is laid out forward from its own start: a
block begins at an offset and its fields follow at increasing displacements.
The tile frame is the one exception. Its displacements are **negative** —
`VALIDATION` at -5, `TILE_INDEX` at -9, `Z_PLANES` at -11 — measured back from
the first byte of the tile stream, which is the byte a `TILE_OFFSETS` entry
addresses.

This is deliberate, and it is what makes the header **optional**:

* **A tile offset always points straight at pixel data.** The entry addresses
  the stream itself, never a header in front of it, so a reader that wants
  bytes gets them with no indirection and no per-tile header to skip.
* **The frame is additive, not structural.** Because it grows backward from
  that anchor, a tile can carry one or not, and the offset that names the
  stream is identical either way. Adding a frame to an existing tile does not
  move the tile.
* **It supplies what the stream cannot.** `TILE_INDEX` is the thing no amount
  of reading a compressed stream can recover, since streams may be written in
  any order — which is what lets a recovery scan put a stream back in the
  right place.

**Consequences for anyone writing one:** `store()` takes the **anchor** — the
first byte of the stream — not the frame's own start, and the runtime builds
its handle the same way (`frame{__base, stream_at, ...}` in `IFE_Runtime.cpp`).
Passing the frame's start writes it five bytes early. That failure is quiet:
a `u40` storing its own position is self-consistent wherever it lands, so the
recovery scan accepts it and the frame appears "covered" while attached to no
stream at all. It was caught once (2026-08-18) only because the misplaced
write happened to land inside a `CIPHER` block and broke validation there.

The frame carries no recovery tag and is found by signature match, so it
appears in `recover_file_structure` and **never** in `generate_file_map` —
the offset graph has no edge that leads to it. Any coverage check that only
walks the graph will report a framed file as unframed.
