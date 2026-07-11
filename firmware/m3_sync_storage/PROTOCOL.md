# Screendeck USB sync protocol (SDC3 v1)

The M3 device exposes one WinUSB bulk function with interface GUID
`{F38C253C-7E95-4F15-A9FD-7BBC31E4F0C4}`. It is a composite USB device with a
separate HID keyboard function; it never exposes the microSD card as mass
storage.

Every little-endian frame is a packed 20-byte header followed by its payload:

| Offset | Field |
| --- | --- |
| 0 | `u32 magic` = `0x33434453` (`SDC3`) |
| 4 | `u8 version` = `1` |
| 5 | `u8 opcode` |
| 6 | `u16 reserved` = `0` |
| 8 | `u32 sequence` |
| 12 | `u32 payload_bytes` (at most 1400) |
| 16 | `u32 payload_crc32` |

The checksum is reflected CRC-32/ISO-HDLC: initial state zero, polynomial
`0xEDB88320`, and final XOR `0xFFFFFFFF`. This is the wire-equivalent of the
firmware's `esp_crc32_le(UINT32_MAX, ...)` call.

Request opcodes are `HELLO=1`, `BEGIN=2`, `CHUNK=3`, `COMMIT=4`, `ABORT=5`,
`STATUS=6`, `DIAG=7`, `MEDIA_BEGIN=8`, `MEDIA_CHUNK=9`, `MEDIA_COMMIT=10`,
and `MEDIA_ABORT=11`. Each response uses `opcode | 0x80` and an 8-byte
payload of `u32 status, u32 value`. The response has the same sequence as its
request.

`BEGIN` contains `u32 total_bundle_bytes, u32 payload_crc32`. If the same
transfer was interrupted, its response value is the persisted byte offset to
resume. `CHUNK` contains `u32 offset, u32 chunk_crc32, bytes[]`; chunks are
strictly in order. `COMMIT` only succeeds after the complete bundle passes
both size and payload checks.

A bundle begins with the packed 16-byte `SDB3` header: magic `0x33424453`,
schema version `1`, header size `16`, total bundle bytes including the header,
and the payload CRC32. Commit moves the verified stage file to an immutable
numbered bundle, then writes an immutable active pointer. Boot selects the
highest valid pointer, so an interrupted commit leaves the earlier pointer
usable.

`MEDIA_BEGIN`, `MEDIA_CHUNK`, and `MEDIA_COMMIT` use the same begin/chunk
wire layouts but operate on a separate media transfer. The device validates the
whole raw MJPEG stream (size, CRC-32, JPEG SOI and EOI boundaries) and then
replaces `/sdcard/screendeck/screensaver.mjpg`. The `HELLO` capability bit
`0x20` indicates this upload path is available.
