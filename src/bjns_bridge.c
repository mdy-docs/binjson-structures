/*
 * bjns_bridge.c — the browser's bj_ns adapter.
 *
 * bjio_posix.c implements the namespace over openat; this implements it
 * over OPFS, where opening a file is ASYNCHRONOUS and bj_ns.open is
 * required to be synchronous. Those cannot both be true at once, and the
 * way out is the discipline bjns.h describes rather than a mechanism:
 *
 *   C plans   -- a pure call returns the names it will need
 *   host opens -- JS awaits each one and registers it under its name
 *   C executes -- one synchronous call, resolving names from that table
 *
 * So `open` here is a LOOKUP, not an open. Every name it can be asked for
 * was named by the immediately preceding plan and has already been opened
 * by the host; a name that was not is BJ_ERR_STATE, which under the
 * discipline is unreachable and therefore a bug rather than a condition
 * to handle.
 *
 * The alternative would be Asyncify or JSPI, letting C block on the
 * promise. Both were rejected for the same reason: neither exists under
 * WASI or native, so the browser would need a different control-flow
 * model from the server -- which is the very problem this whole effort
 * exists to remove.
 *
 * Deletion is DEFERRED. removeEntry is async too, so `remove` queues the
 * name and JS drains the queue once the synchronous call returns. That is
 * safe because every deletion in this codebase happens AFTER an atomic
 * catalog commit -- an undeleted file is an orphan the next sweep
 * collects, never a correctness problem. Ordering a create against a
 * remove would not be safe, so no caller may; BJ_NS_TRUNC exists for
 * that case.
 *
 * Not compiled outside emscripten (see the consumer's build-common.sh),
 * where bjio_posix.c provides the real thing.
 */
#include "bjns.h"
#include "hostio.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>

/*
 * Resolve `name` to an fd the host already opened and registered under
 * this scope, or -1.
 *
 * `Module.bjnsScopes[scope]` is a plain object mapping file name to the
 * fd in Module.bjioHandles -- populated by the host between the plan and
 * execute calls, and cleared afterwards so a stale entry cannot satisfy
 * a later, undeclared request. Deferred unlinks accumulate separately in
 * Module.bjnsPending[scope].
 */
EM_JS(int, bjns_js_lookup, (int scope, const char *name, int name_len), {
    var scopes = Module['bjnsScopes'];
    if (!scopes || !scopes[scope]) return -1;
    /* Decoded here rather than with UTF8ToString: EM_JS bodies do not
     * pull in runtime helpers, and that one is tree-shaken out of this
     * build. HEAPU8 is always in scope, as hostio.c relies on too. */
    var dec = Module['bjnsDecoder'] || (Module['bjnsDecoder'] = new TextDecoder());
    var key = dec.decode(HEAPU8.subarray(name, name + name_len));
    var fd = scopes[scope][key];
    return (fd === undefined) ? -1 : fd;
});

/* Queue a deferred unlink; the host drains after the call returns.
 * Kept in its own map rather than as a reserved key inside the name ->
 * fd table, so a file can be called anything at all. */
EM_JS(void, bjns_js_queue_remove, (int scope, const char *name, int name_len), {
    var pending = Module['bjnsPending'] || (Module['bjnsPending'] = {});
    var list = pending[scope] || (pending[scope] = []);
    var dec = Module['bjnsDecoder'] || (Module['bjnsDecoder'] = new TextDecoder());
    list.push(dec.decode(HEAPU8.subarray(name, name + name_len)));
});

#else /* !__EMSCRIPTEN__ */

/* Present only so an accidental native compilation links; a non-browser
 * host uses bjio_posix.c, which really opens files. */
static int bjns_js_lookup(int scope, const char *name, int name_len) {
    (void)scope; (void)name; (void)name_len; return -1;
}
static void bjns_js_queue_remove(int scope, const char *name, int name_len) {
    (void)scope; (void)name; (void)name_len;
}

#endif /* __EMSCRIPTEN__ */

typedef struct { int scope; } bridge_ns;

static int32_t bns_open(void *ctx, const char *name, uint32_t name_len,
                        uint32_t flags, bj_io *out) {
    if (name_len > 0x7fffffffu) return BJ_ERR_RANGE;
    int fd = bjns_js_lookup(((bridge_ns *)ctx)->scope, name, (int)name_len);
    if (fd < 0) return BJ_ERR_STATE;   /* undeclared name: a plan/execute bug */
    *out = bjio_host(fd);

    /*
     * CREATE and EXCL are the host's and cannot be anything else: they
     * are decisions about a file that does not have a handle yet, and by
     * the time this runs it does. TRUNC is different -- it is a thing you
     * can do TO an open handle, and bj_io has the verb for it.
     *
     * So it is honored here rather than left to the host, because the
     * plan the host opened from is a list of NAMES: it says which files
     * a call will touch, not how each one must be opened. A host cannot
     * apply a flag it was never told. Left undone, an existing file
     * opened for overwrite keeps whatever tail the new contents do not
     * reach -- which is a restored snapshot file with the old database's
     * records still in it.
     */
    if ((flags & BJ_NS_TRUNC) && out->truncate) {
        int32_t e = out->truncate(out->ctx, 0);
        if (e) return e;
    }
    return BJ_OK;
}

static int32_t bns_close(void *ctx, bj_io *io) {
    /* The host opened the handle and the host closes it -- the same
     * ownership hostio.c has always had. Clearing the struct keeps a
     * caller from using it afterwards. */
    (void)ctx;
    if (io) memset(io, 0, sizeof(*io));
    return BJ_OK;
}

static int32_t bns_remove(void *ctx, const char *name, uint32_t name_len) {
    if (name_len > 0x7fffffffu) return BJ_ERR_RANGE;
    bjns_js_queue_remove(((bridge_ns *)ctx)->scope, name, (int)name_len);
    return BJ_OK;
}

/* OPFS has no directory-entry fsync, and no way to ask for one. A
 * successful no-op is the honest answer: the platform makes the
 * durability guarantee the POSIX adapter has to ask for. */
static int32_t bns_sync(void *ctx) { (void)ctx; return BJ_OK; }

int bjns_bridge_open(int scope, bj_ns *out) {
    bridge_ns *b = (bridge_ns *)calloc(1, sizeof(bridge_ns));
    if (!b) return BJ_ERR_OOM;
    b->scope = scope;
    bj_ns ns = {
        .ctx    = b,
        .open   = bns_open,
        .close  = bns_close,
        .remove = bns_remove,
        .sync   = bns_sync,
    };
    *out = ns;
    return BJ_OK;
}

void bjns_bridge_free(bj_ns *ns) {
    if (!ns || !ns->ctx) return;
    free(ns->ctx);
    memset(ns, 0, sizeof(*ns));
}
