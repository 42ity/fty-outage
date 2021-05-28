#pragma once
#include "data.h"
#include <fty_log.h>
#include <malamute.h>

#define TIMEOUT_MS 30000 // wait at least 30 seconds

// hack to allow us to pretend zhash is set
static void* TRUE = const_cast<void*>(reinterpret_cast<const void*>("true"));

typedef struct _s_osrv_t
{
    uint64_t      timeout_ms;
    mlm_client_t* client;
    data_t*       assets;
    zhash_t*      active_alerts;
    char*         state_file;
    uint64_t      default_maintenance_expiration;
    bool          verbose;
} s_osrv_t;

inline void s_osrv_destroy(s_osrv_t** self_p)
{
    assert(self_p);
    if (*self_p) {
        s_osrv_t* self = *self_p;
        zhash_destroy(&self->active_alerts);
        data_destroy(&self->assets);
        mlm_client_destroy(&self->client);
        zstr_free(&self->state_file);
        free(self);
        *self_p = NULL;
    }
}

inline s_osrv_t* s_osrv_new()
{
    s_osrv_t* self = reinterpret_cast<s_osrv_t*>(malloc(sizeof(s_osrv_t)));
    if (self) {
        self->client = mlm_client_new();
        if (self->client)
            self->assets = data_new();
        if (self->assets)
            self->active_alerts = zhash_new();
        if (self->active_alerts) {
            self->timeout_ms                     = TIMEOUT_MS;
            self->state_file                     = NULL;
            self->default_maintenance_expiration = 0;
            self->verbose                        = false;
        } else {
            s_osrv_destroy(&self);
        }
    }
    return self;
}

inline int s_osrv_save(s_osrv_t* self)
{
    assert(self);

    if (!self->state_file) {
        log_warning("There is no state path set-up, can't store the state");
        return -1;
    }

    zconfig_t* root = zconfig_new("root", NULL);
    assert(root);

    zconfig_t* active_alerts = zconfig_new("alerts", root);
    assert(active_alerts);

    size_t i = 0;
    for (void* it = zhash_first(self->active_alerts); it != NULL; it = zhash_next(self->active_alerts)) {
        const char* value = zhash_cursor(self->active_alerts);
        char*       key   = zsys_sprintf("%zu", i++);
        zconfig_put(active_alerts, key, value);
        zstr_free(&key);
    }

    int ret = zconfig_save(root, self->state_file);
    log_debug("outage_actor: save state to %s", self->state_file);
    zconfig_destroy(&root);
    return ret;
}

inline int s_osrv_load(s_osrv_t* self)
{
    assert(self);

    if (!self->state_file) {
        log_warning("There is no state path set-up, can't load the state");
        return -1;
    }

    zconfig_t* root = zconfig_load(self->state_file);
    if (!root) {
        log_error("Can't load configuration from %s: %m", self->state_file);
        return -1;
    }

    zconfig_t* active_alerts = zconfig_locate(root, "alerts");
    if (!active_alerts) {
        log_error("Can't find 'alerts' in %s", self->state_file);
        zconfig_destroy(&root);
        return -1;
    }

    for (zconfig_t* child = zconfig_child(active_alerts); child != NULL; child = zconfig_next(child)) {
        zhash_insert(self->active_alerts, zconfig_value(child), TRUE);
    }

    zconfig_destroy(&root);
    return 0;
}
