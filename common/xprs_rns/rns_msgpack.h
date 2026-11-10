/**
 * @file rns_msgpack.h
 * @brief The msgpack a Reticulum request and its response are written in.
 *
 * RNS packs a request as msgpack([time, path_hash, data]) and its response
 * as msgpack([request_id, data]) (RNS/Link.py); a resource advertisement is
 * a msgpack map. This is the subset those use, in both directions: nil,
 * booleans, integers, floats, strings, binary, arrays and maps. Encoded the
 * way reticulum-dart's lxmf_msgpack.dart encodes (the smallest form of each),
 * so what this writes is byte for byte what the Dart side writes.
 *
 * Pure: no allocation. A reader walks a buffer; a writer fills one and
 * remembers when it ran out of room rather than writing past it.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MP_NIL, MP_BOOL, MP_INT, MP_FLOAT, MP_STR, MP_BIN, MP_ARR, MP_MAP };

typedef struct {
    int      type;
    int64_t  i;          /* MP_INT, MP_BOOL */
    double   f;          /* MP_FLOAT */
    const uint8_t *s;    /* MP_STR, MP_BIN: the bytes, inside the buffer */
    size_t   len;        /* MP_STR, MP_BIN: bytes; MP_ARR, MP_MAP: entries */
} mp_obj_t;

typedef struct { const uint8_t *p; size_t n, i; } mp_rd_t;

static inline void mp_rd_init(mp_rd_t *r, const uint8_t *p, size_t n) { r->p = p; r->n = n; r->i = 0; }

/** One object's head. A string or binary is consumed whole ([o->s] points at
 *  it); an array or a map leaves its entries to the following calls. */
bool mp_next(mp_rd_t *r, mp_obj_t *o);
/** Skip one whole object, nested ones included. */
bool mp_skip(mp_rd_t *r);
/** In a map whose head was just read ([n] entries), find the value under the
 *  string key [key]: positions the reader at it. The reader's position
 *  before the call is the map's first key. */
bool mp_map_find(const mp_rd_t *map, size_t n, const char *key, mp_rd_t *at);

typedef struct { uint8_t *b; size_t cap, n; bool err; } mp_wr_t;

static inline void mp_wr_init(mp_wr_t *w, uint8_t *b, size_t cap) { w->b = b; w->cap = cap; w->n = 0; w->err = false; }

void mp_nil(mp_wr_t *w);
void mp_bool(mp_wr_t *w, bool v);
void mp_uint(mp_wr_t *w, uint64_t v);
void mp_f64(mp_wr_t *w, double v);
void mp_str(mp_wr_t *w, const char *s, size_t len);
void mp_cstr(mp_wr_t *w, const char *s);
void mp_bin(mp_wr_t *w, const uint8_t *b, size_t len);
/** A bin header alone: the bytes are written (or streamed) by the caller. */
void mp_bin_head(mp_wr_t *w, size_t len);
void mp_arr(mp_wr_t *w, size_t n);
void mp_map(mp_wr_t *w, size_t n);
void mp_raw(mp_wr_t *w, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif
