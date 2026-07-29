/*
 * bjns_bridge.h — the browser's bj_ns adapter. See bjns_bridge.c.
 *
 * A "scope" is one directory's worth of pre-opened files, identified by
 * an integer the host chooses. The host populates
 * Module.bjnsScopes[scope] with a name -> fd map between the plan and
 * execute calls, and drains Module.bjnsPending[scope] afterwards.
 */
#ifndef BJNS_BRIDGE_H
#define BJNS_BRIDGE_H

#include "bjns.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A namespace resolving names against the host's table for `scope`.
 * Does not own the scope or its handles. */
int  bjns_bridge_open(int scope, bj_ns *out);
void bjns_bridge_free(bj_ns *ns);

#ifdef __cplusplus
}
#endif

#endif /* BJNS_BRIDGE_H */
