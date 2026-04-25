# DBF Notes

Current DBF support is intentionally tiny.

- Format: dBase III style fixed-length records
- No memo files
- No field parsing helpers yet
- Records are passed around as raw byte arrays
- Host implementation currently uses descriptor I/O

Near-term goal:

- Keep the interface small enough for a CP/M SQL server that must fit
  into a very small memory budget
