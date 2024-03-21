/*  =========================================================================
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

#include <fty_proto.h>
#include <czmq.h>
#include <string>
#include <vector>
#include <cstdint>

////////////////////////////////////////////////////////////////////////////////
///
/// data_t
///
////////////////////////////////////////////////////////////////////////////////

/// opacified structure
typedef struct _data_t data_t;

///  Create a new data
data_t* data_new();

///  Destroy the data
void data_destroy(data_t** self_p);

/// Accessor on asset_expir internal hash list
zhashx_t* data_asset_expir(data_t* self);

/// Get asset friendlyName (ext. name)
const char* data_get_asset_ename(data_t* self, const char* asset_name);

///  Return default expiry time (sec)
uint64_t data_default_expiry(data_t* self);

///  Set default expiry time (sec)
void data_set_default_expiry(data_t* self, uint64_t expiry_sec);

///  Handle deletion/update for a proto asset
///  takes ownership on proto
void data_put(data_t* self, fty_proto_t** proto);

///  Delete source from cache
void data_delete(data_t* self, const char* source);

///  Returns list of nonresponding devices
std::vector<std::string> data_get_dead_devices(data_t* self);

///  update information about expiration time
///  return -1, if data are from future and are ignored as damaging
///  return 0 otherwise
int data_touch_asset(data_t* self, const char* asset_name, uint64_t timestamp, uint64_t ttl, uint64_t now_sec);

/// set/unset asset maintenance mode (choose time=0 to unset)
/// returns 0 if success, else <0
int data_maintenance_asset(data_t* self, const char* asset_name, uint64_t time_sec);
