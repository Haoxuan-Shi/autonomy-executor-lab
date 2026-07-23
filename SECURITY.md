# Security policy

The tool analyzes untrusted CSV and writes a requested trace path. Run it with ordinary user privileges and do not use its output as a safety certification. Report parser crashes, unbounded resource consumption, time overflows, or cases where malformed input is silently accepted to the maintainers.

Input is opened in binary mode. LF/CRLF framing, CTRL-Z, and read failures are
handled explicitly so platform text-mode conventions cannot hide a workload
suffix. Stream identifiers still receive strict UTF-8 and control-character
validation after CSV decoding.
