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
#include "expiration.h"
#include <fty_log.h>

/// it is used as TTL, but in formula we are waiting for ttl*2 ->
/// see expiration_time()
/// so to get a 15 minutes outage default TTL -> we choose the half
#define DEFAULT_ASSET_EXPIRATION_TIME_SEC ((15 * 60) / 2)

///  Structure of data
struct _data_t
{
    zhashx_t* asset_expir;        //!< <asset_name, expiration_t*>
    zhashx_t* asset_enames;       //!< <asset_name => asset_friendlyName> (unicode name)
    uint64_t  default_expiry_sec; //!< default time for the asset, in what asset would be considered as not responding
};

//  --------------------------------------------------------------------------
//  Destroy the data
void data_destroy(data_t** self_p)
{
    if (self_p && (*self_p)) {
        data_t* self = *self_p;
        zhashx_destroy(&self->asset_expir);
        zhashx_destroy(&self->asset_enames);
        memset(self, 0, sizeof(*self));
        free(self);
        *self_p = NULL;
    }
}

//  -----------------------------------------------------------------------
//  Create a new data
data_t* data_new()
{
    data_t* self = reinterpret_cast<data_t*>(malloc(sizeof(data_t)));
    if (self) {
        memset(self, 0, sizeof(*self));

        self->asset_expir = zhashx_new();
        self->asset_enames = zhashx_new();
        if (!(self->asset_expir && self->asset_enames)) {
            data_destroy(&self);
            return NULL;
        }

        self->default_expiry_sec = DEFAULT_ASSET_EXPIRATION_TIME_SEC;

        // map ename<->expiration_t*
        zhashx_set_destructor(self->asset_expir, reinterpret_cast<zhashx_destructor_fn*>(expiration_destroy));
        // map ename<->friendlyName
        zhashx_set_destructor(self->asset_enames, reinterpret_cast<zhashx_destructor_fn*>(zstr_free));
    }

    return self;
}

//  ------------------------------------------------------------------------
zhashx_t* data_asset_expir(data_t* self)
{
    return self ? self->asset_expir : NULL;
}

//  ------------------------------------------------------------------------
const char* data_get_asset_ename(data_t* self, const char* asset_name)
{
    if (self && self->asset_enames && asset_name) {
        void* it = zhashx_lookup(self->asset_enames, asset_name);
        if (it) {
            return reinterpret_cast<const char*>(it);
        }
    }

    return "";
}

//  ------------------------------------------------------------------------
//  Return default number of seconds in that newly added asset would expire
uint64_t data_default_expiry(data_t* self)
{
    return self ? self->default_expiry_sec : 0;
}

//  ------------------------------------------------------------------------
//  Set default number of seconds in that newly added asset would expire
void data_set_default_expiry(data_t* self, uint64_t expiry_sec)
{
    if (self) {
        self->default_expiry_sec = expiry_sec;
    }
}

//  ------------------------------------------------------------------------
//  update information about expiration time
//  return -1, if data are from future and are ignored as damaging
//  return 0 otherwise
int data_touch_asset(data_t* self, const char* asset_name, uint64_t timestamp_sec, uint64_t ttl_sec, uint64_t now_sec)
{
    if (!(self && self->asset_expir && asset_name)) {
        return 0;
    }

    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->asset_expir, asset_name));
    if (!e) { // asset is not known, ignore
        return 0;
    }

    // we know information about this asset, update ttl
    expiration_update_ttl(e, ttl_sec);

    // need to compute new expiration time
    if (timestamp_sec > now_sec) {
        return -1; // in the future!
    }

    expiration_update_last_time_seen(e, timestamp_sec);

    logTrace("Touch {}, last_seen={} s, ttl={} s, expires_at={} s",
        asset_name, expiration_last_time_seen(e), expiration_ttl(e), expiration_time(e));

    return 0;
}

// set/unset asset maintenance mode (choose time=0 to unset)
int data_maintenance_asset(data_t* self, const char* asset_name, uint64_t time_sec)
{
    if (!(self && self->asset_expir && asset_name)) {
        return 0;
    }

    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->asset_expir, asset_name));
    if (!e) { // asset is not known, ignore
        return 0;
    }

    expiration_maintenance_set(e, time_sec);

    return 0;
}

// --------------------------------------------------------------------------
// delete source from data

void data_delete(data_t* self, const char* source)
{
    if (!(self && source)) {
        return;
    }

    if (self->asset_expir) {
        zhashx_delete(self->asset_expir, source);
    }

    if (self->asset_enames) {
        zhashx_delete(self->asset_enames, source);
    }
}

//  ------------------------------------------------------------------------
//  put data (delete, update from proto)
//  take ownership on proto_p

void data_put(data_t* self, fty_proto_t** proto_p)
{
    if (!(self && proto_p && (*proto_p))) {
        return;
    }

    fty_proto_t* proto = *proto_p;

    if (fty_proto_id(proto) != FTY_PROTO_ASSET) {
        fty_proto_destroy(proto_p);
        return;
    }

    const char* asset_name = fty_proto_name(proto);
    const char* operation  = fty_proto_operation(proto);
    const char* status     = fty_proto_aux_string(proto, FTY_PROTO_ASSET_STATUS, "active");
    const char* asset_type = fty_proto_aux_string(proto, FTY_PROTO_ASSET_TYPE, "device");
    const char* sub_type   = fty_proto_aux_string(proto, FTY_PROTO_ASSET_SUBTYPE, "");

    logTrace("Put {}, operation={}, status={}", asset_name, operation, status);

    // remove asset from cache
    if (streq(operation, FTY_PROTO_ASSET_OP_DELETE)
        || streq(status, "nonactive")
        || streq(status, "retired")
    ) {
        logDebug("Delete {}", asset_name);
        data_delete(self, asset_name);
    }
    // other asset operations - add ups, epdu, ats or sensors to the cache if not present
    // note: filter sts which have no measure (for that test device.type which is empty)
    else if (streq(asset_type, "device")
        && (
            streq(sub_type, "ups")
            || streq(sub_type, "epdu")
            || streq(sub_type, "sensor")
            || streq(sub_type, "sensorgpio")
            || (streq(sub_type, "sts") && !streq(fty_proto_ext_string(proto, "device.type", ""), ""))
        )
    ) {
        logDebug("Update {}", asset_name);

        const char* friendlyName = fty_proto_ext_string(proto, "name", NULL);
        if (friendlyName) {
            zhashx_update(self->asset_enames, asset_name, strdup(friendlyName));
        }

        // if this asset is not known yet -> add it to the cache
        expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->asset_expir, asset_name));
        if (!e) {
            e = expiration_new(self->default_expiry_sec);
            if (!e) {
                logError("expiration_new() failed");
            }
            else {
                uint64_t now_sec = uint64_t(zclock_time() / 1000);
                expiration_update_last_time_seen(e, now_sec);

                zhashx_update(self->asset_expir, asset_name, e);

                logDebug("ADD {}, last_seen: {} s, ttl: {} s, expires_at: {} s",
                    asset_name, expiration_last_time_seen(e), expiration_ttl(e), expiration_time(e));
            }
        }
    }

    fty_proto_destroy(proto_p);
}

// --------------------------------------------------------------------------
// get non-responding devices

std::vector<std::string> data_get_dead_devices(data_t* self)
{
    std::vector<std::string> dead_devices;

    if (self && self->asset_expir) {
        uint64_t now_sec = uint64_t(zclock_time() / 1000);
        logDebug("Check dead devices (now: {} s)", now_sec);

        for (void* it = zhashx_first(self->asset_expir); it; it = zhashx_next(self->asset_expir)) {
            auto e = static_cast<expiration_t*>(it);
            const char* asset_name = static_cast<const char*>(zhashx_cursor(self->asset_expir));

            bool is_dead = (expiration_time(e) <= now_sec);
            if (is_dead) {
                dead_devices.emplace_back(asset_name);
                logInfo("{} is down (no metric available)", asset_name);
            }
            else {
                logDebug("{} is alive (remaining: {} s)", asset_name, (expiration_time(e) - now_sec));
            }
        }
    }

    return dead_devices;
}
