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

namespace fty::shm::outage
{

enum Status {
    UNKNOWN = 0,
    INACTIVE,
    ACTIVE
};

// write outage metric for ASSET in shared memory, with STATUS value and TTL_SEC/NOW_SEC (seconds)
// Note: NOW_SEC == 0 means built from zclock_time()
// returns 0 if ok, else <0
int write(const char* asset, Status status, unsigned ttl_sec, uint64_t now_sec);

} // namespace
