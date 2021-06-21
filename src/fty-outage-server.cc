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
    log_debug("Alert '%s' is '%s'", subject, alert_state);
    int rv = mlm_client_send(self->client, subject, &msg);
    if (rv != 0)
        log_error("Cannot send alert on '%s' (mlm_client_send)", source_asset);
    zlist_destroy(&actions);
    zstr_free(&subject);
    zstr_free(&rule_name);
}

// if for asset 'source-asset' the 'outage' alert is tracked
// * publish alert in RESOLVE state for asset 'source-asset'
// * removes alert from the list of the active alerts
static void s_osrv_resolve_alert(s_osrv_t* self, const char* source_asset)
{
    assert(self);
    assert(source_asset);

    if (zhash_lookup(self->active_alerts, source_asset)) {
        log_info("\t\tsend RESOLVED alert for source=%s", source_asset);
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
    int rv = -1;

    assert(self);
    assert(source_asset);

    uint64_t now_sec = uint64_t(zclock_time() / 1000);

    if (zhashx_lookup(self->assets->assets, source_asset)) {

        // The asset is already known
        // so resolve the existing alert if mode == ENABLE_MAINTENANCE
        log_debug(
            "outage: maintenance mode: asset '%s' found, so updating it and resolving current alert", source_asset);

        if (mode == ENABLE_MAINTENANCE)
            s_osrv_resolve_alert(self, source_asset);

        // Note: when mode == DISABLE_MAINTENANCE, restore the default expiration
        rv = data_touch_asset(self->assets, source_asset, now_sec,
            (mode == ENABLE_MAINTENANCE) ? uint64_t(expiration_ttl) : self->assets->default_expiry_sec, now_sec);
        if (rv == -1) {
            // FIXME: use agent name from fty-common
            log_error("outage: failed to %sable maintenance mode for asset '%s'",
                (mode == ENABLE_MAINTENANCE) ? "en" : "dis", source_asset);
        } else
            log_info("outage: maintenance mode %sabled for asset '%s'", (mode == ENABLE_MAINTENANCE) ? "en" : "dis",
                source_asset);
    } else {
        log_debug("outage: maintenance mode: asset '%s' not found, so creating it", source_asset);

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
            log_debug("asset: ADDED name='%s', last_seen=%" PRIu64 "[s], ttl= %" PRIu64 "[s], expires_at=%" PRIu64
                      "[s]",
                source_asset, e->last_time_seen_sec, e->ttl_sec, expiration_get(e));
            zhashx_insert(self->assets->assets, source_asset, e);
            rv = 0;
        }
    }
    log_info("outage: maintenance mode %sabled for asset '%s' with TTL %i", (mode == ENABLE_MAINTENANCE) ? "en" : "dis",
        source_asset, expiration_ttl);
    return rv;
}

// if for asset 'source-asset' the 'outage' alert is NOT tracked
// * publish alert in ACTIVE state for asset 'source-asset'
// * adds alert to the list of the active alerts
static void s_osrv_activate_alert(s_osrv_t* self, const char* source_asset)
{
    assert(self);
    assert(source_asset);

    if (!zhash_lookup(self->active_alerts, source_asset)) {
        log_info("\t\tsend ACTIVE alert for source=%s", source_asset);
        s_osrv_send_alert(self, source_asset, "ACTIVE");
        zhash_insert(self->active_alerts, source_asset, TRUE);
    } else {
        /// XXX: Send the alert nevertheless, unexplained behavior change from last release.
        log_debug("\t\talert already active for source=%s (sending alert anyway)", source_asset);
        s_osrv_send_alert(self, source_asset, "ACTIVE");
    }
}


static void s_osrv_check_dead_devices(s_osrv_t* self)
{
    assert(self);

    log_debug("time to check dead devices");
    zlistx_t* dead_devices = data_get_dead(self->assets);
    if (!dead_devices) {
        log_error("Can't get a list of dead devices (memory error)");
        return;
    }
    log_debug("dead_devices.size=%zu", zlistx_size(dead_devices));
    for (void* it = zlistx_first(dead_devices); it != NULL; it = zlistx_next(dead_devices)) {
        const char* source = reinterpret_cast<const char*>(it);
        log_debug("\tsource=%s", source);
        s_osrv_activate_alert(self, source);
    }
    zlistx_destroy(&dead_devices);
}

static int s_osrv_actor_commands(s_osrv_t* self, zmsg_t** message_p)
{
    assert(self);
    assert(message_p && *message_p);

    zmsg_t* message = *message_p;

    char* command = zmsg_popstr(message);
    if (!command) {
        zmsg_destroy(message_p);
        log_warning("Empty command.");
        return 0;
    }
    log_debug("Command : %s", command);
    if (streq(command, "$TERM")) {
        log_debug("Got $TERM");
        zmsg_destroy(message_p);
        zstr_free(&command);
        return 1;
    } else if (streq(command, "CONNECT")) {
        char* endpoint = zmsg_popstr(message);
        char* name     = zmsg_popstr(message);

        if (endpoint && name) {
            log_debug("outage_actor: CONNECT: %s/%s", endpoint, name);
            int rv = mlm_client_connect(self->client, endpoint, 1000, name);
            if (rv == -1)
                log_error("mlm_client_connect failed\n");
        }

        zstr_free(&endpoint);
        zstr_free(&name);

    } else if (streq(command, "CONSUMER")) {
        char* stream = zmsg_popstr(message);
        char* regex  = zmsg_popstr(message);

        if (stream && regex) {
            log_debug("CONSUMER: %s/%s", stream, regex);
            int rv = mlm_client_set_consumer(self->client, stream, regex);
            if (rv == -1)
                log_error("mlm_set_consumer failed");
        }

        zstr_free(&stream);
        zstr_free(&regex);
    } else if (streq(command, "PRODUCER")) {
        char* stream = zmsg_popstr(message);

        if (stream) {
            log_debug("PRODUCER: %s", stream);
            int rv = mlm_client_set_producer(self->client, stream);
            if (rv == -1)
                log_error("mlm_client_set_producer");
        }
        zstr_free(&stream);
    } else if (streq(command, "TIMEOUT")) {
        char* timeout = zmsg_popstr(message);

        if (timeout) {
            self->timeout_ms = uint64_t(atoll(timeout));
            log_debug("TIMEOUT: \"%s\"/%" PRIu64, timeout, self->timeout_ms);
        }
        zstr_free(&timeout);
    } else if (streq(command, "ASSET-EXPIRY-SEC")) {
        char* timeout = zmsg_popstr(message);

        if (timeout) {
            data_set_default_expiry(self->assets, uint64_t(atol(timeout)));
            log_debug("ASSET-EXPIRY-SEC: \"%s\"/%" PRIu64, timeout, atol(timeout));
        }
        zstr_free(&timeout);
    } else if (streq(command, "STATE-FILE")) {
        char* state_file = zmsg_popstr(message);
        if (state_file) {
            self->state_file = strdup(state_file);
            log_debug("STATE-FILE: %s", state_file);
            int r = s_osrv_load(self);
            if (r != 0)
                log_error("failed to load state file %s: %m", self->state_file);
        }
        zstr_free(&state_file);
    } else if (streq(command, "VERBOSE")) {
        self->verbose = true;
    } else if (streq(command, "DEFAULT_MAINTENANCE_EXPIRATION")) {
        char* maintenance_expiration = zmsg_popstr(message);
        if (maintenance_expiration) {
            self->default_maintenance_expiration = uint64_t(atoi(maintenance_expiration));
            log_debug("DEFAULT_MAINTENANCE_EXPIRATION: %s", maintenance_expiration);
        }
        zstr_free(&maintenance_expiration);
    } else {
        log_error("Unknown actor command: %s.\n", command);
    }

    zstr_free(&command);
    zmsg_destroy(message_p);
    return 0;
}

void metric_processing(fty::shm::shmMetrics& metrics, void* args)
{

    s_osrv_t* self = reinterpret_cast<s_osrv_t*>(args);

    for (auto& element : metrics) {
        const char* is_computed = fty_proto_aux_string(element, "x-cm-count", NULL);
        if (!is_computed) {
            uint64_t    now_sec   = uint64_t(zclock_time() / 1000);
            uint64_t    timestamp = fty_proto_time(element);
            const char* port      = fty_proto_aux_string(element, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

            if (port != NULL) {
                // is it from sensor? yes
                // get sensors attached to the 'asset' on the 'port'! we can have more than 1!
                const char* source = fty_proto_aux_string(element, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
                if (NULL == source) {
                    log_error("Sensor message malformed: found %s='%s' but %s is missing",
                        FTY_PROTO_METRICS_SENSOR_AUX_PORT, port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                    continue;
                }
                log_debug("Sensor '%s' on '%s'/'%s' is still alive", source, fty_proto_name(element), port);
                s_osrv_resolve_alert(self, source);
                int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(element), now_sec);
                if (rv == -1)
                    log_error("asset: name = %s, topic=%s metric is from future! ignore it", source,
                        mlm_client_subject(self->client));
            } else {
                // is it from sensor? no
                const char* source = fty_proto_name(element);
                s_osrv_resolve_alert(self, source);
                int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(element), now_sec);
                if (rv == -1)
                    log_error("asset: name = %s, topic=%s metric is from future! ignore it", source,
                        mlm_client_subject(self->client));
            }
        } else {
            // intentionally left empty
            // so it is metric from agent-cm -> it is not comming from the device itself ->ignore it
        }
    }
}

void outage_metric_polling(zsock_t* pipe, void* args)
{
    zpoller_t* poller = zpoller_new(pipe, NULL);
    zsock_signal(pipe, 0);

    while (!zsys_interrupted) {
        void* which = zpoller_wait(poller, fty_get_polling_interval() * 1000);
        if (zpoller_terminated(poller) || zsys_interrupted) {
            log_info("outage_actor: Terminating.");
            break;
        }
        if (zpoller_expired(poller)) {
            fty::shm::shmMetrics result;
            log_debug("read metrics");
            fty::shm::read_metrics(".*", ".*", result);
            log_debug("i have read %d metric", result.size());
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
}


//  --------------------------------------------------------------------------
//  Handle mailbox messages

static void fty_outage_handle_mailbox(s_osrv_t* self, zmsg_t** msg)
{
    if (self->verbose)
        zmsg_print(*msg);
    if (msg && *msg) {
        char* message_type = zmsg_popstr(*msg);
        if (!message_type) {
            log_warning("Expected message of type REQUEST");
            return;
        }
        char* zuuid = zmsg_popstr(*msg);
        if (!zuuid) {
            log_warning("Expected zuuid");
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
                log_warning("Expected command");
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
                    log_debug("last_str: %s", last_str);
                    // '-' means that it's asset name, otherwise the expiration TTL
                    if (strchr(last_str, '-') == NULL)
                        expiration_ttl = atoi(last_str);
                    zstr_free(&last_str);
                }

                // look for mode 'enable' or 'disable'
                char* mode_str = zmsg_popstr(*msg);
                int   mode     = ENABLE_MAINTENANCE;

                if (mode_str) {

                    log_debug("Maintenance mode: %s", mode_str);

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
                log_warning("'%s': invalid command", command);
                zmsg_addstr(reply, "ERROR");
                zmsg_addstr(reply, "Invalid command");
            }
        } else {
            // message_type is not expected
            log_warning("'%s': invalid message type", message_type);
            zmsg_addstr(reply, "ERROR");
            zmsg_addstr(reply, "Invalid message type");
        }

        if (self->verbose)
            zmsg_print(reply);

        mlm_client_sendto(self->client, sender, subject, NULL, 5000, &reply);
        if (reply) {
            log_error("Could not send message to %s", mlm_client_sender(self->client));
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
    s_osrv_t* self = s_osrv_new();
    assert(self);

    zpoller_t* poller = zpoller_new(pipe, mlm_client_msgpipe(self->client), NULL);
    assert(poller);

    zsock_signal(pipe, 0);
    log_info("outage_actor: Started");
    //    poller timeout
    uint64_t now_ms             = uint64_t(zclock_mono());
    uint64_t last_dead_check_ms = now_ms;
    uint64_t last_save_ms       = now_ms;

    zactor_t* metric_poll = zactor_new(outage_metric_polling, self);
    while (!zsys_interrupted) {
        self->timeout_ms = uint64_t(fty_get_polling_interval() * 1000);
        void* which      = zpoller_wait(poller, int(self->timeout_ms));

        if (which == NULL) {
            if (zpoller_terminated(poller) || zsys_interrupted) {
                log_info("outage_actor: Terminating.");
                break;
            }
        }

        now_ms = uint64_t(zclock_mono());

        // save the state
        if ((now_ms - last_save_ms) > SAVE_INTERVAL_MS) {
            int r = s_osrv_save(self);
            if (r != 0)
                log_error("failed to save state file %s", self->state_file);
            last_save_ms = now_ms;
        }

        // send alerts
        if (zpoller_expired(poller) || (now_ms - last_dead_check_ms) > self->timeout_ms) {
            s_osrv_check_dead_devices(self);
            last_dead_check_ms = uint64_t(zclock_mono());
        }

        if (which == pipe) {
            log_trace("which == pipe");
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
            log_trace("which == mlm_client_msgpipe");

            zmsg_t* message = mlm_client_recv(self->client);
            if (!message)
                break;

            if (!fty_proto_is(message)) {
                if (streq(mlm_client_address(self->client), FTY_PROTO_STREAM_METRICS_UNAVAILABLE)) {
                    char* foo = zmsg_popstr(message);
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
                    log_debug("%s: MAILBOX DELIVER", __func__);
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
                const char* is_computed = fty_proto_aux_string(bmsg, "x-cm-count", NULL);
                if (!is_computed) {
                    uint64_t    now_sec   = uint64_t(zclock_time() / 1000);
                    uint64_t    timestamp = fty_proto_time(bmsg);
                    const char* port      = fty_proto_aux_string(bmsg, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

                    if (port != NULL) {
                        // is it from sensor? yes
                        // get sensors attached to the 'asset' on the 'port'! we can have more then 1!
                        const char* source = fty_proto_aux_string(bmsg, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
                        if (NULL == source) {
                            log_error("Sensor message malformed: found %s='%s' but %s is missing",
                                FTY_PROTO_METRICS_SENSOR_AUX_PORT, port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                            continue;
                        }
                        log_debug("Sensor '%s' on '%s'/'%s' is still alive", source, fty_proto_name(bmsg), port);
                        s_osrv_resolve_alert(self, source);
                        int rv = data_touch_asset(self->assets, source, timestamp, fty_proto_ttl(bmsg), now_sec);
                        if (rv == -1)
                            log_error("asset: name = %s, topic=%s metric is from future! ignore it", source,
                                mlm_client_subject(self->client));
                    } else {
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
                                log_error("asset: name = %s, topic=%s metric is from future! ignore it", source,
                                    mlm_client_subject(self->client));
                        }
                    }
                } else {
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
    if (r != 0)
        log_error("outage_actor: failed to save state file %s: %m", self->state_file);
    s_osrv_destroy(&self);
    log_info("outage_actor: Ended");
}
