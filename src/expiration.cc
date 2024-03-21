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

#include "expiration.h"
#include <fty_log.h>

///  Structure of our class
struct _expiration_t
{
    uint64_t last_time_seen_sec;  //!< time when some metrics were seen for that asset
    uint64_t ttl_sec;             //!< minimal ttl seen for the asset
    uint64_t maintenance_sec;     //!< maintenance timeout if non nul
};

expiration_t* expiration_new(uint64_t default_expiry_sec)
{
    expiration_t* self = reinterpret_cast<expiration_t*>(malloc(sizeof(expiration_t)));
    if (self) {
        memset(self, 0, sizeof(*self));
        self->last_time_seen_sec = 0;
        self->ttl_sec            = default_expiry_sec;
        self->maintenance_sec    = 0;
    }
    return self;
}

void expiration_destroy(expiration_t** self_p)
{
    if (self_p && (*self_p)) {
        expiration_t* self = *self_p;
        free(self);
        *self_p = NULL;
    }
}

// set up last_seen time
// can only prolong time
void expiration_update_last_time_seen(expiration_t* self, uint64_t last_time_seen_sec)
{
    if (!self) {
        return;
    }

    // *only* prolong last_seen
    if (last_time_seen_sec > self->last_time_seen_sec) {
        logTrace("set last_time_seen to {} s", last_time_seen_sec);
        self->last_time_seen_sec = last_time_seen_sec;
    }
}

uint64_t expiration_last_time_seen(expiration_t* self)
{
    return self ? self->last_time_seen_sec : 0;
}

// set up ttl
// can only reduce ttl
void expiration_update_ttl(expiration_t* self, uint64_t ttl_sec)
{
    if (!self) {
        return;
    }

    // *only* reduce ttl
    if (ttl_sec < self->ttl_sec) {
        logTrace("set ttl to {} s", ttl_sec);
        self->ttl_sec = ttl_sec;
    }
}

uint64_t expiration_ttl(expiration_t* self)
{
    return self ? self->ttl_sec : 0;
}

uint64_t expiration_time(expiration_t* self)
{
    if (!self) {
        return 0;
    }

    // time without maintenance
    uint64_t time_sec = self->last_time_seen_sec + (self->ttl_sec * 2);

    if (self->maintenance_sec != 0) { // maintenance enabled?
        if (self->maintenance_sec > time_sec) {
            time_sec = self->maintenance_sec;
        }
        else {
            // outdated, disable maintenance (auto reset)
            logTrace("maintenance mode auto reset");
            self->maintenance_sec = 0;
        }
    }

    return time_sec;
}

void expiration_maintenance_set(expiration_t* self, uint64_t time_sec)
{
    if (self) {
        self->maintenance_sec = time_sec;
    }
}

uint64_t expiration_maintenance(expiration_t* self)
{
    return self ? self->maintenance_sec : 0;
}
