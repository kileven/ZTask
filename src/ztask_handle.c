#include "ztask.h"

#include "ztask_handle.h"
#include "ztask_server.h"
#include <uv.h>

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

struct handle_name {
    char * name;
    uint32_t handle;
};

struct handle_storage {
    uv_rwlock_t lock;

    uint32_t harbor;
    uint32_t handle_index;
    int slot_size;
    struct ztask_context ** slot;

    int name_cap;
    int name_count;
    struct handle_name *name;
};

static struct handle_storage *H = NULL;

uint32_t
ztask_handle_register(struct ztask_context *ctx) {
    struct handle_storage *s = H;

    uv_rwlock_wrlock(&s->lock);

    for (;;) {
        int i;
        for (i = 0; i < s->slot_size; i++) {
            uint32_t handle = (i + s->handle_index) & HANDLE_MASK;
            int hash = handle & (s->slot_size - 1);
            if (s->slot[hash] == NULL) {
                s->slot[hash] = ctx;
                s->handle_index = handle + 1;

                uv_rwlock_wrunlock(&s->lock);

                handle |= s->harbor;
                return handle;
            }
        }
        assert((s->slot_size * 2 - 1) <= HANDLE_MASK);
        struct ztask_context ** new_slot = ztask_malloc(s->slot_size * 2 * sizeof(struct ztask_context *));
        memset(new_slot, 0, s->slot_size * 2 * sizeof(struct ztask_context *));
        for (i = 0; i < s->slot_size; i++) {
            int hash = ztask_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
            assert(new_slot[hash] == NULL);
            new_slot[hash] = s->slot[i];
        }
        ztask_free(s->slot);
        s->slot = new_slot;
        s->slot_size *= 2;
    }
}

int
ztask_handle_retire(uint32_t handle) {
    int ret = 0;
    struct handle_storage *s = H;

    uv_rwlock_wrlock(&s->lock);

    uint32_t hash = handle & (s->slot_size - 1);
    struct ztask_context * ctx = s->slot[hash];

    if (ctx != NULL && ztask_context_handle(ctx) == handle) {
        s->slot[hash] = NULL;
        ret = 1;
        int i;
        int j = 0, n = s->name_count;
        for (i = 0; i < n; ++i) {
            if (s->name[i].handle == handle) {
                ztask_free(s->name[i].name);
                continue;
            }
            else if (i != j) {
                s->name[j] = s->name[i];
            }
            ++j;
        }
        s->name_count = j;
    }
    else {
        ctx = NULL;
    }

    uv_rwlock_wrunlock(&s->lock);

    if (ctx) {
        // release ctx may call ztask_handle_* , so wunlock first.
        ztask_context_release(ctx);
    }

    return ret;
}

void
ztask_handle_retireall() {
    struct handle_storage *s = H;
    for (;;) {
        int n = 0;
        int i;
        for (i = 0; i < s->slot_size; i++) {
            uv_rwlock_rdlock(&s->lock);
            struct ztask_context * ctx = s->slot[i];
            uint32_t handle = 0;
            if (ctx)
                handle = ztask_context_handle(ctx);
            uv_rwlock_rdunlock(&s->lock);
            if (handle != 0) {
                if (ztask_handle_retire(handle)) {
                    ++n;
                }
            }
        }
        if (n == 0)
            return;
    }
}

struct ztask_context *
    ztask_handle_grab(uint32_t handle) {
    struct handle_storage *s = H;
    struct ztask_context * result = NULL;

    uv_rwlock_rdlock(&s->lock);

    uint32_t hash = handle & (s->slot_size - 1);
    struct ztask_context * ctx = s->slot[hash];
    if (ctx && ztask_context_handle(ctx) == handle) {
        result = ctx;
        ztask_context_grab(result);
    }

    uv_rwlock_rdunlock(&s->lock);

    return result;
}

uint32_t
ztask_handle_findname(const char * name) {
    struct handle_storage *s = H;

    uv_rwlock_rdlock(&s->lock);

    uint32_t handle = 0;

    int begin = 0;
    int end = s->name_count - 1;
    while (begin <= end) {
        int mid = (begin + end) / 2;
        struct handle_name *n = &s->name[mid];
        int c = strcmp(n->name, name);
        if (c == 0) {
            handle = n->handle;
            break;
        }
        if (c < 0) {
            begin = mid + 1;
        }
        else {
            end = mid - 1;
        }
    }

    uv_rwlock_rdunlock(&s->lock);

    return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
    if (s->name_count >= s->name_cap) {
        s->name_cap *= 2;
        assert(s->name_cap <= MAX_SLOT_SIZE);
        struct handle_name * n = ztask_malloc(s->name_cap * sizeof(struct handle_name));
        int i;
        for (i = 0; i < before; i++) {
            n[i] = s->name[i];
        }
        for (i = before; i < s->name_count; i++) {
            n[i + 1] = s->name[i];
        }
        ztask_free(s->name);
        s->name = n;
    }
    else {
        int i;
        for (i = s->name_count; i > before; i--) {
            s->name[i] = s->name[i - 1];
        }
    }
    s->name[before].name = name;
    s->name[before].handle = handle;
    s->name_count++;
}

static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
    int begin = 0;
    int end = s->name_count - 1;
    while (begin <= end) {
        int mid = (begin + end) / 2;
        struct handle_name *n = &s->name[mid];
        int c = strcmp(n->name, name);
        if (c == 0) {
            return NULL;
        }
        if (c < 0) {
            begin = mid + 1;
        }
        else {
            end = mid - 1;
        }
    }
    char * result = ztask_strdup(name);

    _insert_name_before(s, result, handle, begin);

    return result;
}

const char *
ztask_handle_namehandle(uint32_t handle, const char *name) {
    uv_rwlock_wrlock(&H->lock);

    const char * ret = _insert_name(H, name, handle);

    uv_rwlock_wrunlock(&H->lock);

    return ret;
}

void
ztask_handle_init(int harbor) {
    assert(H == NULL);
    struct handle_storage * s = ztask_malloc(sizeof(*H));
    s->slot_size = DEFAULT_SLOT_SIZE;
    s->slot = ztask_malloc(s->slot_size * sizeof(struct ztask_context *));
    memset(s->slot, 0, s->slot_size * sizeof(struct ztask_context *));

    uv_rwlock_init(&s->lock);
    // reserve 0 for system
    s->harbor = (uint32_t)(harbor & 0xff) << HANDLE_REMOTE_SHIFT;
    s->handle_index = 1;
    s->name_cap = 2;
    s->name_count = 0;
    s->name = ztask_malloc(s->name_cap * sizeof(struct handle_name));

    H = s;

    // Don't need to free H
}
