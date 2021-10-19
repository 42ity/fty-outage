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


#include "data.h"
#include "fty-outage.h"
#include "fty_common_macros.h"
#include "osrv.h"
#include <fty_log.h>
#include <fty_shm.h>
#include <malamute.h>

#define SAVE_INTERVAL_MS 45 * 60 * 1000 // store state each 45 minutes

// publish 'outage' alert for asset 'source-asset' in state 'alert-state'
static void s_osrv_send_alert(s_osrv_t* self, const char* source_asset, const char* alert_state)
{
    logInfo("SERVER SEND ALERT ===========>");

    assert(self);
    assert(source_asset);
    assert(alert_state);

    zlist_t* actions = zlist_new();
    // FIXME: should be a configurable Settings->Alert!!!
    zlist_append(actions, const_cast<char*>("EMAIL"));
    zlist_append(actions, const_cast<char*>("SMS"));
    char*       rule_name = zsys_sprintf("%s@%s", "outage", source_asset);
    std::string description =
        TRANSLATE_ME("Device %s does not provide expected data. It may be offline or not correctly configured.",
            data_get_asset_ename(self->assets, source_asset));
    zmsg_t* msg     = fty_proto_encode_alert(NULL, // aux
        uint64_t(zclock_time() / 1000),        // unix time (sec.)
        uint32_t(self->timeout_ms * 3 / 1000), // ttl (sec.)
        rule_name,                             // rule_name
        source_asset, alert_state, "CRITICAL", description.c_str(), actions);
    char*   subject = zsys_sprintf("%s/%s@%s", "outage", "CRITICAL", source_asset);
    logDebug("Alert '{}' is '{}'", subject, alert_state);
    int rv = mlm_client_send(self->client, subject, &msg);
    if (rv != 0) {
        logError("Cannot send alert on '{}' (mlm_client_send)", source_asset);
    }
    zlist_destroy(&actions);
    zstr_free(&subject);
    zstr_free(&rule_name);
}

// if for asset 'source-asset' the 'outage' alert is tracked
// * publish alert in RESOLVE state for asset 'source-asset'
// * removes alert from the list of the active alerts
static void s_osrv_resolve_alert(s_osrv_t* self, const char* source_asset)
{
    logInfo("SERVER RESOLVE ALERT {} ===========>", source_asset);

    assert(self);
    assert(source_asset);

    if (zhash_lookup(self->active_alerts, source_asset)) {
        logInfo("\t\tsend RESOLVED alert for source={}", source_asset);
        s_osrv_send_alert(self, source_asset, "RESOLVED");
        zhash_delete(self->active_alerts, source_asset);
    }
}

// switch asset 'source-asset' to maintenance mode
// this implies putting a long TTL, so that no 'outage' alert is generated
// return -1, if operation failed
// return 0 otherwise
static int s_osrv_maintenance_mode(s_osrv_t* self, const char* source_asset, int mode, int expiration_ttl)
{
    logInfo("SERVER MAINTENANCE MODE ===========>");

    int rv = -1;

    assert(self);
    assert(source_asset);

    uint64_t now_sec = uint64_t(zclock_time() / 1000);

    if (zhashx_lookup(self->assets->assets, source_asset)) {
        logInfo("SERVER in lookup ===========>");

        // The asset is already known
        // so resolve the existing alert if mode == ENABLE_MAINTENANCE
        logDebug(
            "outage: maintenance mode: asset '{}' found, so updating it and resolving current alert", source_asset);

        if (mode == ENABLE_MAINTENANCE)
            s_osrv_resolve_alert(self, source_asset);

        // Note: when mode == DISABLE_MAINTENANCE, restore the default expiration
        rv = data_touch_asset(self->assets, source_asset, now_sec,
            (mode == ENABLE_MAINTENANCE) ? uint64_t(expiration_ttl) : self->assets->default_expiry_sec, now_sec);
        if (rv == -1) {
            // FIXME: use agent name from fty-common
            logError("outage: failed to {}able maintenance mode for asset '{}'",
                (mode == ENABLE_MAINTENANCE) ? "en" : "dis", source_asset);
        } else
            logInfo("outage: maintenance mode {}abled for asset '{}'", (mode == ENABLE_MAINTENANCE) ? "en" : "dis",
                source_asset);
    } else {
        logInfo("SERVER in else lookup ===========>");

        logDebug("outage: maintenance mode: asset '{}' not found, so creating it", source_asset);

        // The asset is already known, so add it to the tracking list
        // theoretically, this is only needed when generating the outage alert
        // so not applicable here!
        // zhashx_insert (self->assets->asset_enames, asset_name, (void*) fty_proto_ext_string (proto, "name", ""));

        expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->assets->assets, source_asset));
        if (e == NULL) {
            // FIXME: check if the 2nd param is really needed, seems not used!
            fty_proto_t* msg = fty_proto_new(FTY_PROTO_ASSET);

            e = expiration_new(
                (mode == ENABLE_MAINTENANCE) ? uint64_t(expiration_ttl) : self->assets->default_expiry_sec, &msg);

            expiration_update(e, now_sec);
            logDebug("asset: ADDED name='{}', last_seen={}[s], ttl={}[s], expires_at={}[s]", source_asset,
                e->last_time_seen_sec, e->ttl_sec, expiration_get(e));
            zhashx_insert(self->assets->assets, source_asset, e);
            rv = 0;
        }
    }
    logInfo("outage: maintenance mode {}abled for asset '{}' with TTL {}", (mode == ENABLE_MAINTENANCE) ? "en" : "dis",
        source_asset, expiration_ttl);
    return rv;
}

// if for asset 'source-asset' the 'outage' alert is NOT tracked
// * publish alert in ACTIVE state for asset 'source-asset'
// * adds alert to the list of the active alerts
static void s_osrv_activate_alert(s_osrv_t* self, const char* source_asset)
{
    logInfo("SERVER ACTIVATE ALERT {} ===========>", source_asset);

    assert(self);
    assert(source_asset);

    if (!zhash_lookup(self->active_alerts, source_asset)) {
        logInfo("\t\tsend ACTIVE alert for source={}", source_asset);
        s_osrv_send_alert(self, source_asset, "ACTIVE");
        zhash_insert(self->active_alerts, source_asset, TRUE);
    } else {
        /// XXX: Send the alert nevertheless, unexplained behavior change from last release.
        logDebug("\t\talert already active for source={} (sending alert anyway)", source_asset);
        s_osrv_send_alert(self, source_asset, "ACTIVE");
    }
}


static void s_osrv_check_dead_devices(s_osrv_t* self)
{
    assert(self);

    logDebug("time to check dead devices");
    auto dead_devices = data_get_dead(self->assets);
    /* if (!dead_devices.empty()) {
        logError("Can't get a list of dead devices (memory error)");
        return;
    } */
    logDebug("dead_devices.size={}", dead_devices.size());
    // for (void* it = zlistx_first(dead_devices); it != NULL; it = zlistx_next(dead_devices)) {
    for (const auto& source : dead_devices) {
        // const char* source = reinterpret_cast<const char*>(it);
        logDebug("\tsource={}", source);
        s_osrv_activate_alert(self, source.c_str());
    }
    // zlistx_destroy(&dead_devices);
}

static int s_osrv_actor_commands(s_osrv_t* self, zmsg_t** message_p)
{
    logInfo("SERVER actor commands ===========>");

    assert(self);
    assert(message_p && *message_p);

    zmsg_t* message = *message_p;

    char* command = zmsg_popstr(message);
    logInfo("SERVER command = {} ===========>", command);

    if (!command) {
        zmsg_destroy(message_p);
        logWarn("Empty command.");
        return 0;
    }
    logDebug("Command : {}", command);
    if (streq(command, "$TERM")) {
        logDebug("Got $TERM");
        zmsg_destroy(message_p);
        zstr_free(&command);
        return 1;
    } else if (streq(command, "CONNECT")) {

        char* endpoint = zmsg_popstr(message);
        char* name     = zmsg_popstr(message);

        if (endpoint && name) {
            logDebug("outage_actor: CONNECT: {}/{}", endpoint, name);
            int rv = mlm_client_connect(self->client, endpoint, 1000, name);
            if (rv == -1)
                logError("mlm_client_connect failed\n");
        }

        zstr_free(&endpoint);
        zstr_free(&name);

    } else if (streq(command, "CONSUMER")) {
        char* stream = zmsg_popstr(message);
        char* regex  = zmsg_popstr(message);

        if (stream && regex) {
            logDebug("CONSUMER: {}/{}", stream, regex);
            int rv = mlm_client_set_consumer(self->client, stream, regex);
            if (rv == -1)
                logError("mlm_set_consumer failed");
        }

        zstr_free(&stream);
        zstr_free(&regex);
    } else if (streq(command, "PRODUCER")) {
        char* stream = zmsg_popstr(message);

        if (stream) {
            logDebug("PRODUCER: {}", stream);
            int rv = mlm_client_set_producer(self->client, stream);
            if (rv == -1)
                logError("mlm_client_set_producer");
        }
        zstr_free(&stream);
    } else if (streq(command, "TIMEOUT")) {
        char* timeout = zmsg_popstr(message);

        if (timeout) {
            self->timeout_ms = uint64_t(atoll(timeout));
            logDebug("TIMEOUT: \"{}\"/{}", timeout, self->timeout_ms);
        }
        zstr_free(&timeout);
    } else if (streq(command, "ASSET-EXPIRY-SEC")) {
        char* timeout = zmsg_popstr(message);

        if (timeout) {
            data_set_default_expiry(self->assets, uint64_t(atol(timeout)));
            logDebug("ASSET-EXPIRY-SEC: \"{}\"/{}", timeout, atol(timeout));
        }
        zstr_free(&timeout);
    } else if (streq(command, "STATE-FILE")) {
        char* state_file = zmsg_popstr(message);
        if (state_file) {
            self->state_file = strdup(state_file);
            logDebug("STATE-FILE: {}", state_file);
            int r = s_osrv_load(self);
            if (r != 0)
                logError("failed to load state file {}: %m", self->state_file);
        }
        zstr_free(&state_file);
    } else if (streq(command, "VERBOSE")) {
        self->verbose = true;
    } else if (streq(command, "DEFAULT_MAINTENANCE_EXPIRATION")) {
        char* maintenance_expiration = zmsg_popstr(message);
        if (maintenance_expiration) {
            self->default_maintenance_expiration = uint64_t(atoi(maintenance_expiration));
            logDebug("DEFAULT_MAINTENANCE_EXPIRATION: {}", maintenance_expiration);
        }
        zstr_free(&maintenance_expiration);
    } else {
        logError("Unknown actor command: {}.\n", command);
    }

    zstr_free(&command);
    zmsg_destroy(message_p);
    return 0;
}

void metric_processing(fty::shm::shmMetrics& metrics, void* args)
{
    logInfo("SERVER metric processing, size = {} ===========>", metrics.size());

    s_osrv_t* self = reinterpret_cast<s_osrv_t*>(args);

    for (auto& element : metrics) {
        logInfo("metric process element = {} ===========>", fty_proto_name(element));

        const char* is_computed = fty_proto_aux_string(element, "x-cm-count", NULL);
        if (!is_computed) {
            logInfo("not is computed ===========>");

            uint64_t    now_sec   = uint64_t(zclock_time() / 1000);
            uint64_t    timestamp = fty_proto_time(element);
            const char* port      = fty_proto_aux_string(element, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

            if (port != NULL) {
                logInfo("port exist ===========>");

                // is it from sensor? yes
                // get sensors attached to the 'asset' on the 'port'! we can have more than 1!
                const char* source = fty_proto_aux_string(element, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
                if (NULL == source) {
                    logError("Sensor message malformed: found {}='{}' but {} is missing",
                        FTY_PROTO_METRICS_SENSOR_AUX_PORT, port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                    continue;
                }
                logDebug("Sensor '{}' on '{}'/'{}' is still alive", source, fty_proto_name(element), port);
                s_osrv_resolve_alert(self, source);
                int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(element), now_sec);
                if (rv == -1)
                    logError("asset: name = {}, topic={} metric is from future! ignore it", source,
                        mlm_client_subject(self->client));
            } else {
                logInfo("port not exist  ===========>");

                // is it from sensor? no
                const char* source = fty_proto_name(element);
                s_osrv_resolve_alert(self, source);
                int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(element), now_sec);
                if (rv == -1)
                    logError("asset: name = {}, topic={} metric is from future! ignore it", source,
                        mlm_client_subject(self->client));
            }
        } else {
            logInfo("empty else - WTF ===========>");

            // intentionally left empty
            // so it is metric from agent-cm -> it is not comming from the device itself ->ignore it
        }
    }
}

void outage_metric_polling(zsock_t* pipe, void* args)
{
    logInfo("metric pooling ===========>");

    zpoller_t* poller = zpoller_new(pipe, NULL);
    zsock_signal(pipe, 0);

    while (!zsys_interrupted) {
        void* which = zpoller_wait(poller, fty_get_polling_interval() * 1000);
        if (zpoller_terminated(poller) || zsys_interrupted) {
            logInfo("outage_actor: Terminating.");
            break;
        }
        if (zpoller_expired(poller)) {
            fty::shm::shmMetrics result;
            logDebug("read metrics");
            fty::shm::read_metrics(".*", ".*", result);
            logDebug("i have read {} metric", result.size());
            metric_processing(result, args);
        }
        if (which == pipe) {
            zmsg_t* msg = zmsg_recv(pipe);
            if (msg) {
                char* cmd = zmsg_popstr(msg);
                if (cmd) {
                    if (streq(cmd, "$TERM")) {
                        zstr_free(&cmd);
                        zmsg_destroy(&msg);
                        break;
                    }
                    zstr_free(&cmd);
                }
                zmsg_destroy(&msg);
            }
        }
    }
    zpoller_destroy(&poller);
    logInfo("metric pooling destroyed ===========>");
}


//  --------------------------------------------------------------------------
//  Handle mailbox messages

static void fty_outage_handle_mailbox(s_osrv_t* self, zmsg_t** msg)
{
    logInfo("handle mailbox ===========>");

    if (self->verbose)
        zmsg_print(*msg);
    if (msg && *msg) {
        char* message_type = zmsg_popstr(*msg);
        if (!message_type) {
            logWarn("Expected message of type REQUEST");
            return;
        }
        char* zuuid = zmsg_popstr(*msg);
        if (!zuuid) {
            logWarn("Expected zuuid");
            zstr_free(&message_type);
            return;
        }
        char* command = zmsg_popstr(*msg);
        char* sender  = strdup(mlm_client_sender(self->client));
        char* subject = strdup(mlm_client_subject(self->client));

        // message model always enforce reply
        zmsg_t* reply = zmsg_new();
        zmsg_addstr(reply, zuuid);
        zmsg_addstr(reply, "REPLY");

        if (streq(message_type, "REQUEST")) {
            if (!command) {
                logWarn("Expected command");
                zstr_free(&zuuid);
                zstr_free(&message_type);
                zmsg_addstr(reply, "ERROR");
                zmsg_addstr(reply, "Missing command");
            } else if (streq(command, "MAINTENANCE_MODE")) {
                // * REQUEST/'msg-correlation-id'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration - switch 'asset1'
                // to 'assetN' into maintenance ex: bmsg request fty-outage GET REQUEST 1234 MAINTENANCE_MODE enable
                // ups-9 3600

                // look for expiration_ttl
                int       expiration_ttl = int(self->default_maintenance_expiration);
                zframe_t* last_frame     = zmsg_last(*msg);
                char*     last_str       = NULL;
                if (last_frame) {
                    last_str = zframe_strdup(last_frame);
                    logDebug("last_str: {}", last_str);
                    // '-' means that it's asset name, otherwise the expiration TTL
                    if (strchr(last_str, '-') == NULL)
                        expiration_ttl = atoi(last_str);
                    zstr_free(&last_str);
                }

                // look for mode 'enable' or 'disable'
                char* mode_str = zmsg_popstr(*msg);
                int   mode     = ENABLE_MAINTENANCE;

                if (mode_str) {

                    logDebug("Maintenance mode: {}", mode_str);

                    if ((streq(mode_str, "disable")) || (streq(mode_str, "enable"))) {
                        if (streq(mode_str, "disable")) {
                            mode = DISABLE_MAINTENANCE;
                            // also restore default TTL
                            expiration_ttl = DEFAULT_ASSET_EXPIRATION_TIME_SEC;
                        }
                        // loop on assets...
                        int   rv          = -1;
                        char* maint_asset = zmsg_popstr(*msg);
                        while (maint_asset) {
                            // trim potential ttl (last frame)
                            if (strchr(maint_asset, '-') != NULL)
                                rv = s_osrv_maintenance_mode(self, maint_asset, mode, expiration_ttl);

                            zstr_free(&maint_asset);
                            maint_asset = zmsg_popstr(*msg);
                        }
                        // Process result at the end
                        if (rv == 0)
                            zmsg_addstr(reply, "OK");
                        else {
                            zmsg_addstr(reply, "ERROR");
                            zmsg_addstr(reply, "Command failed");
                        }
                    } else {
                        zmsg_addstr(reply, "ERROR");
                        zmsg_addstr(reply, "Unsupported maintenance mode");
                        zstr_free(&mode_str);
                    }
                    zstr_free(&mode_str);
                } else {
                    zmsg_addstr(reply, "ERROR");
                    zmsg_addstr(reply, "Missing maintenance mode");
                }

            } else {
                // command is not expected
                logWarn("'{}': invalid command", command);
                zmsg_addstr(reply, "ERROR");
                zmsg_addstr(reply, "Invalid command");
            }
        } else {
            // message_type is not expected
            logWarn("'{}': invalid message type", message_type);
            zmsg_addstr(reply, "ERROR");
            zmsg_addstr(reply, "Invalid message type");
        }

        if (self->verbose)
            zmsg_print(reply);

        mlm_client_sendto(self->client, sender, subject, NULL, 5000, &reply);
        if (reply) {
            logError("Could not send message to {}", mlm_client_sender(self->client));
            zmsg_destroy(&reply);
        }

        zstr_free(&subject);
        zstr_free(&sender);
        if (command)
            zstr_free(&command);
        zstr_free(&zuuid);
        zstr_free(&message_type);
        zmsg_destroy(msg);
    }
}

// --------------------------------------------------------------------------
// Create a new fty_outage_server
void fty_outage_server(zsock_t* pipe, void* /*args*/)
{
    logInfo("outage server ===========>");

    s_osrv_t* self = s_osrv_new();
    assert(self);

    zpoller_t* poller = zpoller_new(pipe, mlm_client_msgpipe(self->client), NULL);
    assert(poller);

    zsock_signal(pipe, 0);
    logInfo("outage_actor: Started");
    //    poller timeout
    uint64_t now_ms             = uint64_t(zclock_mono());
    uint64_t last_dead_check_ms = now_ms;
    uint64_t last_save_ms       = now_ms;

    zactor_t* metric_poll = zactor_new(outage_metric_polling, self);
    while (!zsys_interrupted) {
        logInfo("server in while loop +++++ ===========>");

        self->timeout_ms = uint64_t(fty_get_polling_interval() * 1000);
        void* which      = zpoller_wait(poller, int(self->timeout_ms));
        logInfo("while loop after wait +++++ ===========>");

        if (which == NULL) {
            logInfo("which == null +++++ ===========>");

            if (zpoller_terminated(poller) || zsys_interrupted) {
                logInfo("outage_actor: Terminating.");
                break;
            }
        }

        now_ms = uint64_t(zclock_mono());

        // save the state
        if ((now_ms - last_save_ms) > SAVE_INTERVAL_MS) {
            logInfo("while save the state +++++ ===========>");

            int r = s_osrv_save(self);
            if (r != 0)
                logError("failed to save state file {}", self->state_file);
            last_save_ms = now_ms;
        }

        // send alerts
        if (zpoller_expired(poller) || (now_ms - last_dead_check_ms) > self->timeout_ms) {
            logInfo("while zpoller_expired send alert +++++ ===========>");

            s_osrv_check_dead_devices(self);
            last_dead_check_ms = uint64_t(zclock_mono());
        }

        if (which == pipe) {

            logTrace("which == pipe");
            zmsg_t* msg = zmsg_recv(pipe);
            if (!msg)
                break;

            int rv = s_osrv_actor_commands(self, &msg);
            if (rv == 1)
                break;
            continue;
        }
        // react on incoming messages
        else if (which == mlm_client_msgpipe(self->client)) {
            logTrace("which == mlm_client_msgpipe");

            zmsg_t* message = mlm_client_recv(self->client);
            if (!message)
                break;

            if (!fty_proto_is(message)) {
                logInfo("while zpoller_expired send alert +++++ ===========>");

                if (streq(mlm_client_address(self->client), FTY_PROTO_STREAM_METRICS_UNAVAILABLE)) {
                    char* foo = zmsg_popstr(message);
                    logInfo("while pos str = {} +++++ ===========>", foo);

                    if (foo && streq(foo, "METRICUNAVAILABLE")) {
                        zstr_free(&foo);
                        foo                = zmsg_popstr(message); // topic in form aaaa@bbb
                        const char* source = strstr(foo, "@") + 1;
                        s_osrv_resolve_alert(self, source);
                        data_delete(self->assets, source);
                    }
                    zstr_free(&foo);
                } else if (streq(mlm_client_command(self->client), "MAILBOX DELIVER")) {
                    // someone is addressing us directly
                    logDebug("{}: MAILBOX DELIVER", __func__);
                    fty_outage_handle_mailbox(self, &message);
                }
                zmsg_destroy(&message);
                continue;
            }

            fty_proto_t* bmsg = fty_proto_decode(&message);
            if (!bmsg)
                continue;

            // resolve sent alert
            if (fty_proto_id(bmsg) == FTY_PROTO_METRIC ||
                streq(mlm_client_address(self->client), FTY_PROTO_STREAM_METRICS_SENSOR)) {
                logInfo("while resolve sent alert +++++ ===========>");

                const char* is_computed = fty_proto_aux_string(bmsg, "x-cm-count", NULL);
                if (!is_computed) {
                    logInfo("while is not computed +++++ ===========>");

                    uint64_t    now_sec   = uint64_t(zclock_time() / 1000);
                    uint64_t    timestamp = fty_proto_time(bmsg);
                    const char* port      = fty_proto_aux_string(bmsg, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

                    if (port != NULL) {
                        logInfo("while port not null = {} +++++ ===========>", port);

                        // is it from sensor? yes
                        // get sensors attached to the 'asset' on the 'port'! we can have more then 1!
                        const char* source = fty_proto_aux_string(bmsg, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
                        if (NULL == source) {
                            logError("Sensor message malformed: found {}='{}' but {} is missing",
                                FTY_PROTO_METRICS_SENSOR_AUX_PORT, port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                            continue;
                        }
                        logDebug("Sensor '{}' on '{}'/'{}' is still alive", source, fty_proto_name(bmsg), port);
                        s_osrv_resolve_alert(self, source);
                        int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(bmsg), now_sec);
                        if (rv == -1)
                            logError("asset: name = {}, topic={} metric is from future! ignore it", source,
                                mlm_client_subject(self->client));
                    } else {
                        logInfo("while port null  +++++ ===========>");

                        // is it from sensor? no
                        const char* source = fty_proto_name(bmsg);
                        s_osrv_resolve_alert(self, source);
                        const char* operation = fty_proto_operation(bmsg);
                        // hotfix IPMVAL-2713: filter inventory message from sensors which cause the 'outage' alert
                        // activation/deactivation.
                        if (!streq(mlm_client_address(self->client), FTY_PROTO_STREAM_METRICS_SENSOR) ||
                            ((NULL == operation) || !streq(operation, FTY_PROTO_ASSET_OP_INVENTORY))) {
                            int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(bmsg), now_sec);
                            if (rv == -1)
                                logError("asset: name = {}, topic={} metric is from future! ignore it", source,
                                    mlm_client_subject(self->client));
                        }
                    }
                } else {
                logInfo("while empty else - WTF +++++ ===========>");

                    // intentionally left empty
                    // so it is metric from agent-cm -> it is not comming from the device itself ->ignore it
                }
            } else if (fty_proto_id(bmsg) == FTY_PROTO_ASSET) {
                if (streq(fty_proto_operation(bmsg), FTY_PROTO_ASSET_OP_DELETE) ||
                    !streq(fty_proto_aux_string(bmsg, FTY_PROTO_ASSET_STATUS, "active"), "active")) {
                    const char* source = fty_proto_name(bmsg);
                    s_osrv_resolve_alert(self, source);
                }
                data_put(self->assets, &bmsg);
            }
            fty_proto_destroy(&bmsg);
        }
    }
    zactor_destroy(&metric_poll);
    zpoller_destroy(&poller);
    int r = s_osrv_save(self);
    if (r != 0) {
        logError("outage_actor: failed to save state file {}", self->state_file == nullptr ? "null" : self->state_file);
    }
    s_osrv_destroy(&self);
    logInfo("outage_actor: Ended");
}
