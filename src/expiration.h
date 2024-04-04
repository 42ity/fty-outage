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

#include <cstdint>

////////////////////////////////////////////////////////////////////////////////
///
/// expiration_t
///
////////////////////////////////////////////////////////////////////////////////

/// opacified structure
typedef struct _expiration_t expiration_t;

///  Create a new expiration
expiration_t* expiration_new(uint64_t default_expiry_sec);

///  Destroy the expiration
void expiration_destroy(expiration_t** self_p);

///  Update the last time seen
void expiration_update_last_time_seen(expiration_t* self, uint64_t new_time_seen_sec);
uint64_t expiration_last_time_seen(expiration_t* self);

///  Update the ttl
void expiration_update_ttl(expiration_t* self, uint64_t ttl_sec);
uint64_t expiration_ttl(expiration_t* self);

///  Get the expiration time (device death), in seconds
uint64_t expiration_time(expiration_t* self);

///  Handle maintenance timeout (0 if no maintenance)
void expiration_maintenance_set(expiration_t* self, uint64_t time_sec);
uint64_t expiration_maintenance(expiration_t* self);

