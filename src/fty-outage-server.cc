/*  =========================================================================
    fty_outage_server - 42ity outage server

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

#include "fty-outage-server.h"
#include "data.h"
#include "expiration.h"
#include "outage-metric.h"

#include <fty_common_macros.h>
#include <fty_common_agents.h>
#include <fty_log.h>
#include <fty_shm.h>
#include <malamute.h>

#define SAVE_INTERVAL_MS (45 * 60 * 1000) // store state each 45 minutes

#define DISABLE_MAINTENANCE 0
#define ENABLE_MAINTENANCE  1

//  --------------------------------------------------------------------------
// hack to allow us to pretend zhash is set
static void* VOID_TRUE = const_cast<void*>(reinterpret_cast<const void*>("true"));

/// osrv_t structure
struct osrv_t
{
    mlm_client_t* client;
    data_t*       data;
    zhash_t*      active_alerts;
    char*         state_file;
    uint64_t      default_maintenance_expiration; // sec
    uint64_t      timeout_ms;
    bool          verbose;
};

//  --------------------------------------------------------------------------
/// destroy osrv_t object
static void s_osrv_destroy(osrv_t** self_p)
{
    if (self_p && (*self_p)) {
        osrv_t* self = *self_p;
        zhash_destroy(&self->active_alerts);
        data_destroy(&self->data);
        mlm_client_destroy(&self->client);
        zstr_free(&self->state_file);
        free(self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
/// create osrv_t object
static osrv_t* s_osrv_new()
{
    osrv_t* self = reinterpret_cast<osrv_t*>(malloc(sizeof(osrv_t)));
    if (self) {
        memset(self, 0, sizeof(*self));
        self->client = mlm_client_new();
        if (self->client) {
            self->data = data_new();
        }
        if (self->data) {
            self->active_alerts = zhash_new();
        }
        if (self->active_alerts) {
            self->state_file                     = NULL;
            self->timeout_ms                     = uint64_t(fty_get_polling_interval()) * 1000;
            self->default_maintenance_expiration = 60; //sec
            self->verbose                        = false;
        }
        else {
            s_osrv_destroy(&self);
        }
    }
    return self;
}

//  --------------------------------------------------------------------------
/// save active_alerts to file
/// returns 0 if success, else <0
static int s_osrv_save(osrv_t* self)
{
    if (!self) {
        return -1;
    }

    if (!self->state_file) {
        logWarn("There is no state path set-up, can't store the state");
        return -1;
    }

    zconfig_t* root = zconfig_new("root", NULL);
    assert(root);

    zconfig_t* active_alerts = zconfig_new("alerts", root);
    assert(active_alerts);

    size_t i = 0;
    for (void* it = zhash_first(self->active_alerts); it; it = zhash_next(self->active_alerts)) {
        const char* value = zhash_cursor(self->active_alerts);
        char*       key   = zsys_sprintf("%zu", i);
        zconfig_put(active_alerts, key, value);
        zstr_free(&key);
        i++;
    }

    int ret = zconfig_save(root, self->state_file);
    zconfig_destroy(&root);

    logDebug("Save state to {}", self->state_file);
    return ret;
}

//  --------------------------------------------------------------------------
/// read active_alerts from file
/// returns 0 if success, else <0
static int s_osrv_load(osrv_t* self)
{
    if (!self) {
        return -1;
    }

    if (!self->state_file) {
        logWarn("There is no state path set-up, can't load the state");
        return -1;
    }

    logInfo("Load state from {}", self->state_file);
    zconfig_t* root = zconfig_load(self->state_file);
    if (!root) {
        logError("Can't load state from {} ({})", self->state_file, strerror(errno));
        return -1;
    }

    zconfig_t* active_alerts = zconfig_locate(root, "alerts");
    if (!active_alerts) {
        logError("Can't find 'alerts' in {}", self->state_file);
        zconfig_destroy(&root);
        return -1;
    }

    for (zconfig_t* child = zconfig_child(active_alerts); child; child = zconfig_next(child)) {
        zhash_insert(self->active_alerts, zconfig_value(child), VOID_TRUE);
    }

    zconfig_destroy(&root);
    return 0;
}

//  --------------------------------------------------------------------------
/// publish 'outage' alert for asset 'source-asset' in state 'alert-state'
static void s_osrv_send_alert(osrv_t* self, const char* source_asset, const char* alert_state)
{
    if (!(self && source_asset && alert_state)) {
        logDebug("bad args");
        return;
    }

    // should be a configurable Settings->Alert
    zlist_t* actions = zlist_new();
    zlist_append(actions, const_cast<char*>("EMAIL"));
    zlist_append(actions, const_cast<char*>("SMS"));

    char* rule_name = zsys_sprintf("%s@%s", "outage", source_asset);

    const char* friendlyName = data_get_asset_ename(self->data, source_asset);
    std::string description =
        TRANSLATE_ME("Device %s does not provide expected data. It may be offline or not correctly configured.",
            friendlyName);

    zmsg_t* msg = fty_proto_encode_alert(
        NULL, // aux
        uint64_t(zclock_time() / 1000),        // now, unix time (sec.)
        uint32_t(self->timeout_ms * 3 / 1000), // ttl (sec.)
        rule_name,                             // rule_name
        source_asset,
        alert_state, //ACTIVE, RESOLVED
        "CRITICAL",
        description.c_str(),
        actions);

    char* subject = zsys_sprintf("%s/%s@%s", "outage", "CRITICAL", source_asset);

    logInfo("Send alert {} {}", subject, alert_state);

    int r = mlm_client_send(self->client, subject, &msg);
    if (r != 0) {
        logError("Cannot send outage alert on '{}'", source_asset);
    }

    zlist_destroy(&actions);
    zstr_free(&subject);
    zstr_free(&rule_name);
    zmsg_destroy(&msg);
}

//  --------------------------------------------------------------------------
/// if for asset 'source-asset' the 'outage' alert is tracked
/// * publish alert in RESOLVE state for asset 'source-asset'
/// * removes alert from the list of the active alerts
static void s_osrv_resolve_alert(osrv_t* self, const char* source_asset)
{
    if (!(self && source_asset)) {
        logDebug("bad args");
        return;
    }

    if (zhash_lookup(self->active_alerts, source_asset)) {
        s_osrv_send_alert(self, source_asset, "RESOLVED");
        zhash_delete(self->active_alerts, source_asset);
    }
}

//  --------------------------------------------------------------------------
/// switch asset 'source-asset' to maintenance mode
/// this implies putting a long TTL, so that no 'outage' alert is generated
/// mode is in {ENABLE_MAINTENANCE, DISABLE_MAINTENANCE}
/// returns 0 if ok, else <0
static int s_osrv_maintenance_mode(osrv_t* self, const char* source_asset, int mode, uint64_t expiration_ttl_sec)
{
    if (!(self && self->data && source_asset)) {
        logDebug("bad args");
        return -1;
    }

    const char* mode_s = (mode == ENABLE_MAINTENANCE) ? "ENABLE" : "DISABLE";

    logInfo("{}: {} maintenance mode (expiration_ttl: {} s)", source_asset, mode_s, expiration_ttl_sec);

    uint64_t now_sec = uint64_t(zclock_time() / 1000);

    // get data expiration entry (create if none)
    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(data_asset_expir(self->data), source_asset));
    if (!e) {
        e = expiration_new(data_default_expiry(self->data));
        if (!e) {
            log_error("expiration_new() failed");
            return -1;
        }
        // insert
        zhashx_insert(data_asset_expir(self->data), source_asset, e);
    }

    // update/set last_seen
    expiration_update_last_time_seen(e, now_sec);

    // resolve ongoing alert if any
    s_osrv_resolve_alert(self, source_asset);

    // set/unset maintenance mode
    if (mode == ENABLE_MAINTENANCE) {
        data_maintenance_asset(self->data, source_asset, now_sec + expiration_ttl_sec);
    }
    else {
        data_maintenance_asset(self->data, source_asset, 0);
        expiration_update_ttl(e, expiration_ttl_sec);
    }

    return 0;
}

//  --------------------------------------------------------------------------
/// if for asset 'source-asset' the 'outage' alert is NOT tracked
/// * publish alert in ACTIVE state for asset 'source-asset'
/// * adds alert to the list of the active alerts
static void s_osrv_activate_alert(osrv_t* self, const char* source_asset)
{
    if (!(self && source_asset)) {
        logDebug("bad args");
        return;
    }

    if (!zhash_lookup(self->active_alerts, source_asset)) {
        zhash_insert(self->active_alerts, source_asset, VOID_TRUE);
    }

    s_osrv_send_alert(self, source_asset, "ACTIVE");
}

//  --------------------------------------------------------------------------
//
static void s_osrv_check_dead_devices(osrv_t* self)
{
    if (!self) {
        logDebug("bad args");
        return;
    }

    uint64_t now_sec = uint64_t(zclock_time() / 1000);

    std::vector<std::string> devices = data_get_dead_devices(self->data, now_sec);
    logDebug("Dead devices (size: {})", devices.size());
    for (const auto& asset_name : devices) {
        s_osrv_activate_alert(self, asset_name.c_str());
    }
}

//  --------------------------------------------------------------------------
// req asset agent for assets (re)publication on stream
static void s_osrv_assets_republish(osrv_t* self)
{
    if (!(self && self->client)) {
        logDebug("bad args");
        return;
    }

    const char* subject = "REPUBLISH";
    zmsg_t* msg = zmsg_new();
    int r = mlm_client_sendto(self->client, AGENT_FTY_ASSET, subject, NULL, 5000, &msg);
    zmsg_destroy(&msg);
    //no response expected

    if (r == 0) {
        logInfo("Request {}/{} succeeded", AGENT_FTY_ASSET, subject);
    }
    else {
        logError("Request {}/{} failed", AGENT_FTY_ASSET, subject);
    }
}

//  --------------------------------------------------------------------------
//
static void s_outage_metric_poller_process(osrv_t* self)
{
    if (!self) {
        logDebug("bad args");
        return;
    }

    // get all metrics available
    fty::shm::shmMetrics metrics;
    fty::shm::read_metrics(".*", ".*", metrics);

    uint64_t now_sec = uint64_t(zclock_time() / 1000);

    std::vector<std::string> aliveAssets;

    for (auto& metric : metrics) {
        const char* is_computed = fty_proto_aux_string(metric, "x-cm-count", NULL);
        if (is_computed) {
            continue; // ignore computed metrics
        }

        const char* asset_name = fty_proto_name(metric);

        // sensor exception
        const char* port = fty_proto_aux_string(metric, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);
        if (port) {
            // get sensors attached to the 'asset' on the 'port'! we can have more than 1!
            asset_name = fty_proto_aux_string(metric, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
            if (!asset_name) {
                logWarn("Sensor malformed: found {}='{}' but {} is missing",
                    FTY_PROTO_METRICS_SENSOR_AUX_PORT, port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
            }
        }

        if (asset_name) {
            logDebug("{} is alive (type: {}, time: {}, ttl: {})", asset_name,
            fty_proto_type(metric), fty_proto_time(metric), fty_proto_ttl(metric));

            uint64_t timestamp_sec = fty_proto_time(metric);
            uint64_t ttl_sec = fty_proto_ttl(metric);
            int r = data_touch_asset(self->data, asset_name, timestamp_sec, ttl_sec, now_sec);
            if (r != 0) {
                logWarn("{} metric is from future!", asset_name);
            }

            s_osrv_resolve_alert(self, asset_name);

            // asset is alive
            if (std::find(aliveAssets.begin(), aliveAssets.end(), asset_name) == aliveAssets.end()) {
                aliveAssets.push_back(asset_name);
            }
        }
    }

    // update outage metrics
    now_sec = uint64_t(zclock_time() / 1000);
    unsigned ttl_sec = unsigned(2 * fty_get_polling_interval()) - 1;
    std::vector<std::string> allAssets{data_get_all_devices(self->data)};
    for (const auto& asset_name : allAssets) {
        bool dead = std::find(aliveAssets.begin(), aliveAssets.end(), asset_name) == aliveAssets.end();
        using namespace fty::shm;
        outage::write(asset_name.c_str(), dead ? outage::Status::ACTIVE : outage::Status::INACTIVE, ttl_sec, now_sec);
    }
}

//  --------------------------------------------------------------------------
//
static void s_outage_metric_poller(zsock_t* pipe, void* args)
{
    const char* actor_name = "fty-outage-metric";

    osrv_t* osrv = reinterpret_cast<osrv_t*>(args);
    if (!osrv) {
        logError("{}: invalid args", actor_name);
        return;
    }

    zpoller_t* poller = zpoller_new(pipe, NULL);
    if (!poller) {
        logError("{}: zpoller_new failed", actor_name);
        return;
    }

    zsock_signal(pipe, 0);

    logInfo("{}: Started", actor_name);

    while (!zsys_interrupted) {
        int timeout_ms = fty_get_polling_interval() * 1000;
        void* which = zpoller_wait(poller, timeout_ms);

        if (!which) {
            if (zpoller_terminated(poller) || zsys_interrupted) {
                break;
            }

            if (zpoller_expired(poller)) {
                logDebug("{}: ticking...", actor_name);
                s_outage_metric_poller_process(osrv);
            }
        }
        else if (which == pipe) {
            zmsg_t* msg = zmsg_recv(pipe);
            char* cmd = zmsg_popstr(msg);
            bool term = (cmd && streq(cmd, "$TERM"));
            zstr_free(&cmd);
            zmsg_destroy(&msg);
            if (term) {
                break;
            }
        }
    }

    zpoller_destroy(&poller);

    logInfo("{}: Ended", actor_name);
}

//  --------------------------------------------------------------------------
// handle server commands
// take ownership on message_p
// returns 1 if $TERM, else 0
static int s_osrv_handle_commands(osrv_t* self, zmsg_t** message_p)
{
    if (!(self && message_p && (*message_p))) {
        logDebug("bad args");
        return 0;
    }

    zmsg_t* message = *message_p;
    char* command = zmsg_popstr(message);

    int ret = 0;

    if (!command) {
        logWarn("Empty command.");
    }
    else if (streq(command, "$TERM")) {
        logTrace("{}", command);
        ret = 1;
    }
    else if (streq(command, "CONNECT")) {
        char* endpoint = zmsg_popstr(message);
        char* address  = zmsg_popstr(message);
        if (endpoint && address) {
            logDebug("{}: endpoint: {}, address: {}", command, endpoint, address);
            int r = mlm_client_connect(self->client, endpoint, 1000, address);
            if (r != 0) {
                logError("mlm_client_connect failed ({}/{})", endpoint, address);
            }
        }
        zstr_free(&endpoint);
        zstr_free(&address);
    }
    else if (streq(command, "CONSUMER")) {
        char* stream = zmsg_popstr(message);
        char* filter = zmsg_popstr(message);
        if (stream && filter) {
            logDebug("{}: {}/{}", command, stream, filter);
            int r = mlm_client_set_consumer(self->client, stream, filter);
            if (r != 0) {
                logError("mlm_set_consumer failed ({}/{})", stream, filter);
            }
        }
        zstr_free(&stream);
        zstr_free(&filter);
    }
    else if (streq(command, "PRODUCER")) {
        char* stream = zmsg_popstr(message);
        if (stream) {
            logDebug("{}: {}", command, stream);
            int r = mlm_client_set_producer(self->client, stream);
            if (r != 0) {
                logError("mlm_client_set_producer ({})", stream);
            }
        }
        zstr_free(&stream);
    }
    else if (streq(command, "STATE_FILE")) {
        char* state_file = zmsg_popstr(message);
        if (state_file) {
            logDebug("{}: {}", command, state_file);
            zstr_free(&self->state_file);
            self->state_file = strdup(state_file);
            int r = s_osrv_load(self);
            if (r != 0) {
                logError("Failed to load state file {}", self->state_file);
            }
        }
        zstr_free(&state_file);
    }
    else if (streq(command, "DEFAULT_MAINTENANCE_EXPIRATION_SEC")) {
        char* expiry = zmsg_popstr(message);
        if (expiry) {
            uint64_t value = uint64_t(atoi(expiry));
            self->default_maintenance_expiration = value;
            logDebug("{}: {} s", command, value);
        }
        zstr_free(&expiry);
    }
    else if (streq(command, "ASSET_EXPIRY_SEC")) { // Unit test
        char* expiry = zmsg_popstr(message);
        if (expiry) {
            uint64_t value = uint64_t(atol(expiry));
            data_set_default_expiry(self->data, value);
            logDebug("{}: {} s", command, value);
        }
        zstr_free(&expiry);
    }
    else if (streq(command, "VERBOSE")) {
        self->verbose = true;
        logDebug("{}: true", command);
    }
    else {
        logError("Unknown command: {}", command);
    }

    zstr_free(&command);
    zmsg_destroy(message_p);

    return ret; // 1 if $TERM, else 0
}

//  --------------------------------------------------------------------------
//  Handle mailbox messages
//  take ownership on msg

static void s_osrv_handle_mailbox(osrv_t* self, zmsg_t** msg)
{
    if (!(self && msg && (*msg))) {
        logError("invalid arguments");
        return;
    }

    if (self->verbose) {
        zmsg_print(*msg);
    }

    char* message_type = zmsg_popstr(*msg);
    if (!message_type) {
        logError("Expected message type");
        zmsg_destroy(msg);
        return;
    }

    char* zuuid = zmsg_popstr(*msg);
    if (!zuuid) {
        logError("Expected zuuid");
        zstr_free(&message_type);
        zmsg_destroy(msg);
        return;
    }

    // message model always enforce reply
    zmsg_t* reply = zmsg_new();
    zmsg_addstr(reply, zuuid);
    zmsg_addstr(reply, "REPLY");

    const char* sender  = mlm_client_sender(self->client);
    const char* subject = mlm_client_subject(self->client);

    char* command = zmsg_popstr(*msg);

    logDebug("Mailbox: {}/{}", message_type, command);

    if (!streq(message_type, "REQUEST")) {
        // message_type is not expected
        logWarn("'{}': invalid message type", message_type);
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr(reply, "Invalid message type");
    }
    else if (!command) {
        logWarn("Expected command");
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr(reply, "Missing command");
    }
    else if (streq(command, "MAINTENANCE_MODE")) {
        // * REQUEST/'msg-correlation-id'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration - switch 'asset1'
        // to 'assetN' into maintenance
        // ex: bmsg request fty-outage GET REQUEST 1234 MAINTENANCE_MODE enable ups-9 3600

        // look for expiration_ttl (seconds)
        uint64_t expiration_ttl_sec = self->default_maintenance_expiration;
        {
            zframe_t* last_frame = zmsg_last(*msg);
            if (last_frame) {
                char* last_str = zframe_strdup(last_frame);
                logTrace("last_str: {}", last_str);
                // '-' means that it's asset name, otherwise the expiration TTL
                if (!strchr(last_str, '-')) {
                    expiration_ttl_sec = uint64_t(atoi(last_str));
                }
                zstr_free(&last_str);
            }
        }

        // look for mode 'enable' or 'disable'
        char* mode_str = zmsg_popstr(*msg);
        if (!mode_str) {
            zmsg_addstr(reply, "ERROR");
            zmsg_addstr(reply, "Missing maintenance mode");
        }
        else if ((streq(mode_str, "disable")) || (streq(mode_str, "enable"))) {
            int mode = ENABLE_MAINTENANCE;
            if (streq(mode_str, "disable")) {
                mode = DISABLE_MAINTENANCE;
                // restore default TTL
                expiration_ttl_sec = data_default_expiry(self->data);
            }

            // loop on assets...
            bool success = true;
            char* asset = zmsg_popstr(*msg);
            while (asset) {
                // ignore potential ttl (last frame)
                if (strchr(asset, '-')) {
                    int r = s_osrv_maintenance_mode(self, asset, mode, expiration_ttl_sec);
                    success &= (r == 0);
                }
                zstr_free(&asset);
                asset = zmsg_popstr(*msg);
            }

            // Process result at the end
            if (success) {
                zmsg_addstr(reply, "OK");
            }
            else {
                zmsg_addstr(reply, "ERROR");
                zmsg_addstr(reply, "Command failed");
            }
        }
        else {
            zmsg_addstr(reply, "ERROR");
            zmsg_addstr(reply, "Unsupported maintenance mode");
        }
        zstr_free(&mode_str);
    }
    else {
        // invalid command
        logWarn("'{}': invalid command", command);
        zmsg_addstr(reply, "ERROR");
        zmsg_addstr(reply, "Invalid command");
    }

    if (self->verbose) {
        zmsg_print(reply);
    }

    int r = mlm_client_sendto(self->client, sender, subject, NULL, 5000, &reply);
    zmsg_destroy(&reply);
    if (r != 0) {
        logError("Could not send message to {}", sender);
    }

    zstr_free(&command);
    zstr_free(&zuuid);
    zstr_free(&message_type);

    zmsg_destroy(msg);
}

// --------------------------------------------------------------------------
// fty_outage server actor

void fty_outage_server(zsock_t* pipe, void* args)
{
    const char* actor_name = static_cast<char*>(args);

    osrv_t* self = s_osrv_new();
    if (!self) {
        logError("{}: s_osrv_new failed", actor_name);
        return;
    }

    zpoller_t* poller = zpoller_new(pipe, mlm_client_msgpipe(self->client), NULL);
    if (!poller) {
        logError("{}: zpoller_new failed", actor_name);
        s_osrv_destroy(&self);
        return;
    }

    zactor_t* metric_poller = zactor_new(s_outage_metric_poller, self);
    if (!metric_poller) {
        logError("{}: metric_poller actor creation failed", actor_name);
        zpoller_destroy(&poller);
        s_osrv_destroy(&self);
        return;
    }

    zsock_signal(pipe, 0);

    logInfo("{}: Started", actor_name);

    uint64_t now_ms = uint64_t(zclock_mono());
    uint64_t last_dead_check_ms = now_ms;
    uint64_t last_save_ms = now_ms;
    bool republish_assets = true;

    while (!zsys_interrupted) {
        self->timeout_ms = uint64_t(fty_get_polling_interval() * 1000);
        void* which = zpoller_wait(poller, int(self->timeout_ms));

        now_ms = uint64_t(zclock_mono());

        if (!which) {
            if (zpoller_terminated(poller) || zsys_interrupted) {
                break; // $TERM
            }

            if (zpoller_expired(poller)) {
                // ask to republish assets if the service is available
                if (republish_assets && mlm_client_connected(self->client)) {
                    republish_assets = false; // once
                    s_osrv_assets_republish(self);
                }

                // save the state
                if ((now_ms - last_save_ms) > SAVE_INTERVAL_MS) {
                    int r = s_osrv_save(self);
                    last_save_ms = uint64_t(zclock_mono());
                    if (r != 0) {
                        logError("{}: failed to save state file", actor_name);
                    }
                }
            }
        }

        // send alerts on dead devices
        if ((now_ms - last_dead_check_ms) > self->timeout_ms) {
            s_osrv_check_dead_devices(self);
            last_dead_check_ms = uint64_t(zclock_mono());
        }

        if (which == pipe) {
            zmsg_t* message = zmsg_recv(pipe);
            int r = s_osrv_handle_commands(self, &message);
            zmsg_destroy(&message);
            if (r == 1) {
                break; // $TERM
            }
        }
        else if (which == mlm_client_msgpipe(self->client)) {
            // react on incoming messages
            zmsg_t* message = mlm_client_recv(self->client);
            const char* cmd = mlm_client_command(self->client);

            if (streq(cmd, "STREAM DELIVER")) {
                const char* address = mlm_client_address(self->client);

                if (streq(address, FTY_PROTO_STREAM_METRICS_UNAVAILABLE)) {
                    char* aux = zmsg_popstr(message);
                    if (aux && streq(aux, "METRICUNAVAILABLE")) {
                        zstr_free(&aux);
                        aux = zmsg_popstr(message); // topic in form aaaa@bbb
                        if (aux && strstr(aux, "@")) {
                            logDebug("{}/METRICUNAVAILABLE {}", address, aux);
                            const char* asset_name = strstr(aux, "@") + 1;
                            s_osrv_resolve_alert(self, asset_name);
                            data_delete(self->data, asset_name);
                        }
                    }
                    zstr_free(&aux);
                }
                else { // assume from FTY_PROTO_STREAM_ASSETS
                    fty_proto_t* proto = fty_proto_decode(&message);
                    if (proto && (fty_proto_id(proto) == FTY_PROTO_ASSET))
                    {
                        const char* operation = fty_proto_operation(proto);
                        const char* status = fty_proto_aux_string(proto, FTY_PROTO_ASSET_STATUS, "active");

                        if (streq(operation, FTY_PROTO_ASSET_OP_DELETE)
                            || !streq(status, "active")
                        )
                        {
                            const char* asset_name = fty_proto_name(proto);
                            s_osrv_resolve_alert(self, asset_name);
                        }

                        data_put(self->data, &proto);
                    }
                    fty_proto_destroy(&proto);
                }
            }
            else if (streq(cmd, "MAILBOX DELIVER")) {
                // someone is addressing us directly
                s_osrv_handle_mailbox(self, &message);
            }

            zmsg_destroy(&message);
        }
    }

    int r = s_osrv_save(self);
    if (r != 0) {
        logError("{}: failed to save state file", actor_name);
    }

    zactor_destroy(&metric_poller);
    zpoller_destroy(&poller);
    s_osrv_destroy(&self);

    logInfo("{}: Ended", actor_name);
}
