/*
 * Declares private helpers for exact IEEE binary64 key packing and
 * comparison used by dBase III compatible numeric and date NDX keys.
 * The implementation avoids host floating-point types so GCC and SDCC
 * builds can produce the same on-disk key bytes from decimal DBF text.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef ndx_binary64_h
#define ndx_binary64_h

/*
 * Packs one DBF-style numeric text into the little-endian IEEE binary64
 * bytes used by dBase III numeric NDX keys. Leading and trailing spaces
 * are accepted. Returns zero on success and -1 on parse or range error.
 */
int ndx_binary64_from_numeric_text(unsigned char *key, const char *text,
    unsigned short length);

/*
 * Packs one YYYYMMDD date into the little-endian IEEE binary64 bytes
 * used by dBase III date NDX keys. Returns zero on success and -1 on
 * parse or range error.
 */
int ndx_binary64_from_date_text(unsigned char *key, const char *text,
    unsigned short length);

/*
 * Compares two little-endian IEEE binary64 finite values exactly and
 * returns -1, 0, or 1 in numeric order.
 */
int ndx_binary64_compare(const unsigned char *left,
    const unsigned char *right);

#endif
