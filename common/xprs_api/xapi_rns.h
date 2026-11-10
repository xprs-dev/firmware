/*
 * xapi_rns -- the station's visitor API over a Reticulum link (spec/API-HTTP.md
 * "The same API over a Reticulum link"): /xprs/api and /xprs/prove, served on
 * links to the station's xprs.wapp destination through xprsrns_serve(). See
 * xapi_rns.c.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rns_serve.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register both paths with the Reticulum bearer. Call at boot, after
 *  xprsrns_init() and once the HTTP API is up (it is what /xprs/api calls). */
void xapi_rns_start(void);

/** The handlers, for a host that serves them its own way. */
bool xapi_rns_api(const uint8_t *data, size_t len, const uint8_t link_id[16], rns_src_t **out);
bool xapi_rns_prove(const uint8_t *data, size_t len, const uint8_t link_id[16], rns_src_t **out);

#ifdef __cplusplus
}
#endif
