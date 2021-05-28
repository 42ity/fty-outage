/*  =========================================================================
    data - Data

    Copyright (C) 2014 - 2020 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

#include "data.h"
#include <fty_log.h>

expiration_t* expiration_new(uint64_t default_expiry_sec, fty_proto_t** msg_p)
{
    assert(msg_p);
    expiration_t* self = reinterpret_cast<expiration_t*>(zmalloc(sizeof(expiration_t)));
    if (self) {
        self->ttl_sec = default_expiry_sec;
        self->msg     = *msg_p;
        *msg_p        = NULL;
    }
    return self;
}

void expiration_destroy(expiration_t** self_p)
{
    assert(self_p);
    if (*self_p) {
        expiration_t* self = *self_p;
        fty_proto_destroy(&self->msg);
        free(self);
        *self_p = NULL;
    }
}

// set up new expected expiration time, given last seen time
// this function can only prolong exiration_time
void expiration_update(expiration_t* self, uint64_t new_time_seen_sec)
{
    assert(self);
    // this will ensure, that we will not have 'experiation' time moving backwards!
    // Situation: at 03:33 metric with 24h average comes with 'time' = 00:00
    // ttl is 5 minutes -> new expiration date would be 00:05 BUT now already 3:33 !!
    // So we will create false alert!
    // This 'if' is a guard for this situation!
    if (new_time_seen_sec > self->last_time_seen_sec)
        self->last_time_seen_sec = new_time_seen_sec;
}

void expiration_update_ttl(expiration_t* self, uint64_t proposed_ttl)
{
    assert(self);
    // ATTENTION: if minimum ttl for some asset is greater than DEFAULT_ASSET_EXPIRATION_TIME_SEC
    // it will be sending alerts every DEFAULT_ASSET_EXPIRATION_TIME_SEC

    // logic: we are looking for the minimum ttl
    if (self->ttl_sec > proposed_ttl) {
        self->ttl_sec = proposed_ttl;
    }
}

uint64_t expiration_get(expiration_t* self)
{
    assert(self);
    return self->last_time_seen_sec + self->ttl_sec * 2;
}

void ename_destroy(void** ptr)
{
    free(*ptr);
}

//  --------------------------------------------------------------------------
//  Destroy the data
void data_destroy(data_t** self_p)
{
    assert(self_p);
    if (*self_p) {
        data_t* self = *self_p;
        zhashx_destroy(&self->assets);
        zhashx_destroy(&self->asset_enames);
        free(self);
        *self_p = NULL;
    }
}

//  -----------------------------------------------------------------------
//  Create a new data
data_t* data_new(void)
{
    data_t* self = reinterpret_cast<data_t*>(zmalloc(sizeof(data_t)));
    if (self) {
        self->asset_enames = zhashx_new();
        if (!self->asset_enames) {
            data_destroy(&self);
            return NULL;
        }
        zhashx_set_destructor(self->asset_enames, reinterpret_cast<zhashx_destructor_fn*>(ename_destroy));

        self->assets = zhashx_new();
        if (self->assets) {
            self->default_expiry_sec = DEFAULT_ASSET_EXPIRATION_TIME_SEC;
            zhashx_set_destructor(self->assets, reinterpret_cast<zhashx_destructor_fn*>(expiration_destroy));
        } else
            data_destroy(&self);
    }
    return self;
}

const char* data_get_asset_ename(data_t* self, const char* asset_name)
{
    return reinterpret_cast<const char*>(zhashx_lookup(self->asset_enames, asset_name));
}

//  ------------------------------------------------------------------------
//  Return default number of seconds in that newly added asset would expire
uint64_t data_default_expiry(data_t* self)
{
    assert(self);
    return self->default_expiry_sec;
}

//  ------------------------------------------------------------------------
//  Set default number of seconds in that newly added asset would expire
void data_set_default_expiry(data_t* self, uint64_t expiry_sec)
{
    assert(self);
    self->default_expiry_sec = expiry_sec;
}

//  ------------------------------------------------------------------------
//  update information about expiration time
//  return -1, if data are from future and are ignored as damaging
//  return 0 otherwise
int data_touch_asset(data_t* self, const char* asset_name, uint64_t timestamp, uint64_t ttl, uint64_t now_sec)
{
    assert(self);
    assert(asset_name);

    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->assets, asset_name));
    if (e == NULL) {
        // asset is not known -> we are not interested in this asset -> do nothing
        return 0;
    }

    // we know information about this asset
    // try to update ttl
    expiration_update_ttl(e, ttl);
    // need to compute new expiration time
    if (timestamp > now_sec)
        return -1;
    else {
        expiration_update(e, timestamp);
        log_debug("asset: INFO UPDATED name='%s', last_seen=%" PRIu64 "[s], ttl= %" PRIu64 "[s], expires_at=%" PRIu64
                  "[s]",
            asset_name, e->last_time_seen_sec, e->ttl_sec, expiration_get(e));
    }
    return 0;
}

//  ------------------------------------------------------------------------
//  put data
void data_put(data_t* self, fty_proto_t** proto_p)
{
    assert(self);
    assert(proto_p);

    fty_proto_t* proto = *proto_p;
    if (proto == NULL)
        return;

    if (fty_proto_id(proto) != FTY_PROTO_ASSET) {
        fty_proto_destroy(proto_p);
        return;
    }

    const char* operation  = fty_proto_operation(proto);
    const char* asset_name = fty_proto_name(proto);

    log_debug("Received asset: name=%s, operation=%s", asset_name, operation);

    // remove asset from cache
    const char* sub_type = fty_proto_aux_string(proto, FTY_PROTO_ASSET_SUBTYPE, "");
    if (streq(operation, FTY_PROTO_ASSET_OP_DELETE) ||
        streq(fty_proto_aux_string(proto, FTY_PROTO_ASSET_STATUS, ""), "retired") ||
        streq(fty_proto_aux_string(proto, FTY_PROTO_ASSET_STATUS, ""), "nonactive")) {
        data_delete(self, asset_name);
        log_debug("asset: DELETED name=%s, operation=%s", asset_name, operation);
        fty_proto_destroy(proto_p);
    } else
        // other asset operations - add ups, epdu, ats or sensors to the cache if not present
        // note: filter sts which have no measure (for that test device.type which is empty)
        if (streq(fty_proto_aux_string(proto, FTY_PROTO_ASSET_TYPE, ""), "device") &&
            (streq(sub_type, "ups") || streq(sub_type, "epdu") || streq(sub_type, "sensor") ||
                streq(sub_type, "sensorgpio") ||
                (streq(sub_type, "sts") && !streq(fty_proto_ext_string(proto, "device.type", ""), "")))) {

        zhashx_update(self->asset_enames, asset_name, strdup(fty_proto_ext_string(proto, "name", "")));

        // this asset is not known yet -> add it to the cache
        expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->assets, asset_name));
        if (e == NULL) {
            e = expiration_new(self->default_expiry_sec, proto_p);

            uint64_t now_sec = uint64_t(zclock_time() / 1000);
            expiration_update(e, now_sec);
            log_debug("asset: ADDED name='%s', last_seen=%" PRIu64 "[s], ttl= %" PRIu64 "[s], expires_at=%" PRIu64
                      "[s]",
                asset_name, e->last_time_seen_sec, e->ttl_sec, expiration_get(e));
            zhashx_update(self->assets, asset_name, e);
        } else {
            fty_proto_destroy(proto_p);
            // intentionally left empty
            // So, if we already knew this asset -> nothing to do
        }
    } else {
        fty_proto_destroy(proto_p);
    }
}

// --------------------------------------------------------------------------
// delete from cache
void data_delete(data_t* self, const char* source)
{
    assert(self);
    assert(source);

    zhashx_delete(self->assets, source);
}

// --------------------------------------------------------------------------
// RC3 ports are labeled by 9, 10, ... but internaly we use TH1, TH2, ...
char* convert_port(const char* old_port)
{
    if (streq(old_port, "9"))
        return const_cast<char*>("TH1");
    else if (streq(old_port, "10"))
        return const_cast<char*>("TH2");
    else if (streq(old_port, "11"))
        return const_cast<char*>("TH3");
    else if (streq(old_port, "12"))
        return const_cast<char*>("TH4");
    else
        return const_cast<char*>("");
}

// --------------------------------------------------------------------------
// get non-responding devices
zlistx_t* data_get_dead(data_t* self)
{
    assert(self);
    // list of devices
    zlistx_t* dead = zlistx_new();

    uint64_t now_sec = uint64_t(zclock_time() / 1000);
    log_debug("now=%" PRIu64 "s", now_sec);
    for (expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_first(self->assets)); e != NULL;
         e               = reinterpret_cast<expiration_t*>(zhashx_next(self->assets))) {
        void* asset_name = const_cast<void*>(zhashx_cursor(self->assets));
        log_debug("asset: name=%s, ttl=%" PRIu64 ", expires_at=%" PRIu64, asset_name, e->ttl_sec, expiration_get(e));
        if (expiration_get(e) <= now_sec) {
            assert(zlistx_add_start(dead, asset_name));
        }
    }

    return dead;
}
