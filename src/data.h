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

#pragma once

#include "fty-outage.h"
#include <fty_proto.h>

/// it is used as TTL, but in formula we are waiting for ttl*2 ->
/// so if we here would have 15 minutes-> the first alert will come in 30 minutes
#define DEFAULT_ASSET_EXPIRATION_TIME_SEC 15 * 60 / 2

///  Structure of our class
struct _data_t
{
    zhashx_t* assets;             //!< asset_name => expiration time [s]
    zhashx_t* asset_enames;       //!< asset iname => asset ename (unicode name)
    uint64_t  default_expiry_sec; //!< default time for the asset, in what asset would be considered as not responding
};

typedef struct _data_t data_t;

///  Create a new data
data_t* data_new(void);

///  Destroy the data
void data_destroy(data_t** self_p);

/// get asset unicode name
const char* data_get_asset_ename(data_t* self, const char* asset_name);

///  Return default number of seconds in that newly added asset would expire
uint64_t data_default_expiry(data_t* self);

///  Set default number of seconds in that newly added asset would expire
void data_set_default_expiry(data_t* self, uint64_t expiry_sec);

///  calculates metric expiration time for each asset  takes owneship of the message
void data_put(data_t* self, fty_proto_t** proto);

///  delete from cache
void data_delete(data_t* self, const char* source);

///  Returns list of nonresponding devices, zlistx entries are refereces
zlistx_t* data_get_dead(data_t* self);

///  update information about expiration time
///  return -1, if data are from future and are ignored as damaging
///  return 0 otherwise
int data_touch_asset(data_t* self, const char* asset_name, uint64_t timestamp, uint64_t ttl, uint64_t now_sec);

///  Self test of this class
void data_test(bool verbose);

///  Structure of our class
typedef struct _expiration_t
{
    uint64_t     ttl_sec;            //!< minimal ttl seen for some asset
    uint64_t     last_time_seen_sec; //!< time when  some metrics were seen for this asset
    fty_proto_t* msg;                //!< asset representation
} expiration_t;

///  Create a new expiration
expiration_t* expiration_new(uint64_t default_expiry_sec, fty_proto_t** msg_p);

///  Destroy the expiration
void expiration_destroy(expiration_t** self_p);

///  Update the expiration
void expiration_update(expiration_t* self, uint64_t new_time_seen_sec);

///  Update the expiration TTL
void expiration_update_ttl(expiration_t* self, uint64_t proposed_ttl);

///  Get the expiration TTL
uint64_t expiration_get(expiration_t* self);
