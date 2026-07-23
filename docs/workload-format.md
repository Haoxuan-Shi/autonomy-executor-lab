# Workload format

The input is UTF-8 CSV with this exact header:

```text
stream,sequence,release_ms,execution_ms,deadline_ms,source_ms,max_age_ms,priority
```

Lower numeric priority means more urgent. Deadlines are relative to release. Freshness is checked as `dispatch time - source time <= max_age_ms`. Source time may precede release but may not follow it. Execution and deadline must be positive; other times must be non-negative. Stream identifier rules are detailed below.

Each non-empty physical data line is exactly one eight-field record. A field may be
unquoted, in which case it may not contain a quote, or quoted starting at the field
boundary. Inside a quoted field, a literal quote is represented by two quotes. The
closing quote must be followed immediately by a comma or the end of the record.
Bare, misplaced, or unclosed quotes are rejected. Multiline workload records are not
supported.

The reader opens the file in binary mode and recognizes LF or CRLF record
terminators itself. A final record may omit its LF, but a bare or embedded
carriage return is invalid. CTRL-Z (`0x1A`) is rejected anywhere in the input;
it is never interpreted as an end-of-file marker. Read failures are distinct
from clean end of file and cannot authorize a parsed prefix.

## Stream identifiers

A stream identifier must be non-empty, at most 256 bytes after CSV decoding, and encoded
as well-formed UTF-8. Only shortest-form encodings of Unicode scalar values are accepted:
surrogate code points, truncated sequences, malformed continuations, and values above
U+10FFFF are invalid.

Identifiers may contain printable ASCII, including U+0022 (`"`), and multibyte Unicode
characters. To encode U+0022 in a workload, quote the whole field and double each literal
quote; for example, `"camera ""front"""` decodes to `camera "front"`.

U+002C (comma) remains prohibited after CSV decoding as an identifier-level compatibility
rule, even when the input field is correctly quoted. The C0 and C1 control ranges from
U+0000 through U+001F and U+007F through U+009F are also unsupported; this includes NUL,
tab, carriage return, and line feed.

Identifiers are preserved as their original UTF-8 bytes and are not Unicode-normalized.
Consequently, canonically equivalent spellings with different byte sequences remain
distinct stream identifiers and produce different deterministic trace fingerprints.

## Trace CSV encoding

Trace CSV begins with `trace_ordinal`, the monotonic event-order identifier, and then
preserves the decoded text bytes used by the simulation and fingerprint.
Every emitted text field (`stream`, `outcome`, and `diagnosis`) is passed through the
same CSV encoder. Fields containing a comma, quote, carriage return, or line feed are
quoted, and embedded quotes are doubled. CSV serialization does not participate in or
change `sha256-v2:` trace fingerprint construction.
