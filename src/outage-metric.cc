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

#include "outage-metric.h"
#include <fty_log.h>
#include <fty_proto.h>
#include <fty_shm.h>
#include <cstdint>

namespace fty::shm::outage
{

static const char* TYPE = "outage"; // outage@asset

static const char* statusStr(Status status)
{
    switch (status) {
        case Status::INACTIVE: return "INACTIVE";
        case Status::ACTIVE: return "ACTIVE";
        default:;
    }
    return "UNKNOWN";
}

// write metric in shared memory
// returns 0 if ok, else <0
int write(const char* asset, Status status, unsigned ttl_sec, uint64_t now_sec_)
{
    if (!(asset && (*asset))) {
        logError("Invalid asset argument");
        return -1;
    }

    const char* value = statusStr(status);
    const uint64_t now_sec = (now_sec_ != 0) ? now_sec_ : uint64_t(zclock_time() / 1000); // sec

    zmsg_t* msg = fty_proto_encode_metric(NULL, now_sec, ttl_sec, TYPE, asset, value, "" /*unit*/);
    if (!msg) {
        logError("fty_proto_encode_metric() failed: {}@{} (value: {})", TYPE, asset, value);
        return -1;
    }

    fty_proto_t* proto = fty_proto_decode(&msg);
    zmsg_destroy(&msg);

    // mark the metric as 'computed' (see s_outage_metric_poller_process())
    fty_proto_aux_insert(proto, "x-cm-count", "%d", 0);

    int r = fty_shm_write_metric_proto(proto);
    fty_proto_destroy(&proto);
    if (r != 0) {
        logError("fty_shm_write_metric_proto() failed: {}@{} (value: {})", TYPE, asset, value);
        return -1;
    }

    logDebug("{}@{}/{} (ttl={}s)", TYPE, asset, value, std::to_string(ttl_sec));
    return 0; // ok
}

} // namespace
