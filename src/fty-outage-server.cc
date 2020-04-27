/*  =========================================================================
    fty_outage_server - 42ity outage server

    Copyright (C) 2014 - 2019 Eaton

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

/*
@header
    fty_outage_server - 42ity outage server
@discuss
@end
*/
#define TIMEOUT_MS 30000   //wait at least 30 seconds
#define SAVE_INTERVAL_MS 45*60*1000 // store state each 45 minutes

#include "fty_outage_classes.h"
#include "data.h"
#include "fty_common_macros.h"

static void *TRUE = (void*) "true";   // hack to allow us to pretend zhash is set

typedef struct _s_osrv_t {
    uint64_t timeout_ms;
    mlm_client_t *client;
    data_t *assets;
    zhash_t *active_alerts;
    char *state_file;
    uint64_t default_maintenance_expiration;
    bool verbose;
} s_osrv_t;

static void
s_osrv_destroy (s_osrv_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        s_osrv_t *self = *self_p;
        zhash_destroy (&self->active_alerts);
        data_destroy (&self->assets);
        mlm_client_destroy (&self->client);
        zstr_free (&self->state_file);
        free (self);
        *self_p = NULL;
    }
}

static s_osrv_t *
s_osrv_new ()
{
    s_osrv_t *self = (s_osrv_t*) zmalloc (sizeof (s_osrv_t));
    if (self) {
        self->client = mlm_client_new ();
        if (self->client)
            self->assets = data_new ();
        if (self->assets)
            self->active_alerts = zhash_new ();
        if (self->active_alerts) {
            self->timeout_ms = TIMEOUT_MS;
            self->state_file = NULL;
            self->default_maintenance_expiration = 0;
        } else {
            s_osrv_destroy (&self);
        }
        self->verbose = false;
    }
    return self;
}

// publish 'outage' alert for asset 'source-asset' in state 'alert-state'
static void
s_osrv_send_alert (s_osrv_t* self, const char* source_asset, const char* alert_state)
{
    assert (self);
    assert (source_asset);
    assert (alert_state);

    zlist_t *actions = zlist_new ();
    // FIXME: should be a configurable Settings->Alert!!!
    zlist_append(actions, (void *) "EMAIL");
    zlist_append(actions, (void *) "SMS");
    char *rule_name = zsys_sprintf ("%s@%s","outage",source_asset);
    std::string description = TRANSLATE_ME("Device %s does not provide expected data. It may be offline or not correctly configured.", data_get_asset_ename (self->assets, source_asset));
    zmsg_t *msg = fty_proto_encode_alert (
            NULL, // aux
            zclock_time() / 1000, // unix time (sec.)
            self->timeout_ms * 3 / 1000, // ttl (sec.)
            rule_name, // rule_name
            source_asset,
            alert_state,
            "CRITICAL",
            description.c_str(),
            actions);
    char *subject = zsys_sprintf ("%s/%s@%s",
        "outage",
        "CRITICAL",
        source_asset);
    log_debug ("Alert '%s' is '%s'", subject, alert_state);
    int rv = mlm_client_send (self->client, subject, &msg);
    if ( rv != 0 )
        log_error ("Cannot send alert on '%s' (mlm_client_send)", source_asset);
    zlist_destroy(&actions);
    zstr_free (&subject);
    zstr_free (&rule_name);
}

// if for asset 'source-asset' the 'outage' alert is tracked
// * publish alert in RESOLVE state for asset 'source-asset'
// * removes alert from the list of the active alerts
static void
s_osrv_resolve_alert (s_osrv_t* self, const char* source_asset)
{
    assert (self);
    assert (source_asset);

    if (zhash_lookup (self->active_alerts, source_asset)) {
        log_info ("\t\tsend RESOLVED alert for source=%s", source_asset);
        s_osrv_send_alert (self, source_asset, "RESOLVED");
        zhash_delete (self->active_alerts, source_asset);
    }
}

// switch asset 'source-asset' to maintenance mode
// this implies putting a long TTL, so that no 'outage' alert is generated
// return -1, if operation failed
// return 0 otherwise
static int
s_osrv_maintenance_mode (s_osrv_t* self, const char* source_asset, int mode, int expiration_ttl)
{
    int rv = -1;

    assert (self);
    assert (source_asset);

    uint64_t now_sec = zclock_time() / 1000;

    if (zhashx_lookup (self->assets->assets, source_asset)) {

        // The asset is already known
        // so resolve the existing alert if mode == ENABLE_MAINTENANCE
        log_debug ("outage: maintenance mode: asset '%s' found, so updating it and resolving current alert", source_asset);

        if (mode == ENABLE_MAINTENANCE)
            s_osrv_resolve_alert (self, source_asset);

        // Note: when mode == DISABLE_MAINTENANCE, restore the default expiration
        rv = data_touch_asset (self->assets, source_asset, now_sec,
                               (mode==ENABLE_MAINTENANCE)?expiration_ttl:self->assets->default_expiry_sec,
                               now_sec);
        if ( rv == -1 ) {
            // FIXME: use agent name from fty-common
            log_error ("outage: failed to %sable maintenance mode for asset '%s'",
                       (mode==ENABLE_MAINTENANCE)?"en":"dis", source_asset);
        }
        else
            log_info ("outage: maintenance mode %sabled for asset '%s'",
                       (mode==ENABLE_MAINTENANCE)?"en":"dis", source_asset);
    }
    else {
        log_debug ("outage: maintenance mode: asset '%s' not found, so creating it", source_asset);

        // The asset is already known, so add it to the tracking list
        // theoretically, this is only needed when generating the outage alert
        // so not applicable here!
        // zhashx_insert (self->assets->asset_enames, asset_name, (void*) fty_proto_ext_string (proto, "name", ""));

        expiration_t *e = (expiration_t *) zhashx_lookup (self->assets->assets, source_asset);
        if ( e == NULL ) {
            // FIXME: check if the 2nd param is really needed, seems not used!
            fty_proto_t *msg = fty_proto_new (FTY_PROTO_ASSET);
            e = expiration_new ((mode==ENABLE_MAINTENANCE)?expiration_ttl:self->assets->default_expiry_sec, &msg);
            expiration_update (e, now_sec);
            log_debug ("asset: ADDED name='%s', last_seen=%" PRIu64 "[s], ttl= %" PRIu64 "[s], expires_at=%" PRIu64 "[s]", source_asset, e->last_time_seen_sec, e->ttl_sec, expiration_get (e));
            zhashx_insert (self->assets->assets, source_asset, e);
            rv = 0;
        }
    }
    log_info ("outage: maintenance mode %sabled for asset '%s' with TTL %i",
               (mode==ENABLE_MAINTENANCE)?"en":"dis", source_asset, expiration_ttl);
    return rv;
}

// if for asset 'source-asset' the 'outage' alert is NOT tracked
// * publish alert in ACTIVE state for asset 'source-asset'
// * adds alert to the list of the active alerts
static void
s_osrv_activate_alert (s_osrv_t* self, const char* source_asset)
{
    assert (self);
    assert (source_asset);

    if ( !zhash_lookup (self->active_alerts, source_asset)) {
        log_info ("\t\tsend ACTIVE alert for source=%s", source_asset);
        s_osrv_send_alert (self, source_asset, "ACTIVE");
        zhash_insert (self->active_alerts, source_asset, TRUE);
    }
    else
        log_debug ("\t\talert already active for source=%s", source_asset);
}

static int
s_osrv_save (s_osrv_t *self)
{
    assert (self);

    if (!self->state_file) {
        log_warning ("There is no state path set-up, can't store the state");
        return -1;
    }

    zconfig_t *root = zconfig_new ("root", NULL);
    assert (root);

    zconfig_t *active_alerts = zconfig_new ("alerts", root);
    assert (active_alerts);

    size_t i = 0;
    for (void*  it = zhash_first (self->active_alerts);
                it != NULL;
                it = zhash_next (self->active_alerts))
    {
        const char *value = (const char*) zhash_cursor (self->active_alerts);
        char *key = zsys_sprintf ("%zu", i++);
        zconfig_put (active_alerts, key, value);
        zstr_free (&key);
    }

    int ret = zconfig_save (root, self->state_file);
    log_debug ("outage_actor: save state to %s", self->state_file);
    zconfig_destroy (&root);
    return ret;
}

static int
s_osrv_load (s_osrv_t *self)
{
    assert (self);

    if (!self->state_file) {
        log_warning ("There is no state path set-up, can't load the state");
        return -1;
    }

    zconfig_t *root = zconfig_load (self->state_file);
    if (!root) {
        log_error ("Can't load configuration from %s: %m", self->state_file);
        return -1;
    }

    zconfig_t *active_alerts = zconfig_locate (root, "alerts");
    if (!active_alerts) {
        log_error ("Can't find 'alerts' in %s", self->state_file);
        zconfig_destroy (&root);
        return -1;
    }

    for (zconfig_t *child = zconfig_child (active_alerts);
                    child != NULL;
                    child = zconfig_next (child))
    {
        zhash_insert (self->active_alerts, zconfig_value (child), TRUE);
    }

    zconfig_destroy (&root);
    return 0;
}

static void
s_osrv_check_dead_devices (s_osrv_t *self)
{
    assert (self);

    log_debug ("time to check dead devices");
    zlistx_t *dead_devices = data_get_dead (self->assets);
    if ( !dead_devices ) {
        log_error ("Can't get a list of dead devices (memory error)");
        return;
    }
    log_debug ("dead_devices.size=%zu", zlistx_size (dead_devices));
    for (void *it = zlistx_first (dead_devices);
            it != NULL;
            it = zlistx_next (dead_devices))
    {
        const char* source = (const char*) it;
        log_debug ("\tsource=%s", source);
        s_osrv_activate_alert (self, source);
    }
    zlistx_destroy (&dead_devices);
}

/*
 * return values :
 * 1 - $TERM recieved
 * 0 - message processed and deleted
 */

static int
s_osrv_actor_commands (s_osrv_t* self, zmsg_t **message_p)
{
    assert (self);
    assert(message_p && *message_p);

    zmsg_t *message =  *message_p;

    char *command = zmsg_popstr(message);
    if (!command) {
        zmsg_destroy (message_p);
        log_warning ("Empty command.");
        return 0;
    }
    log_debug("Command : %s",command);
    if (streq(command, "$TERM")) {
        log_debug ("Got $TERM");
        zmsg_destroy (message_p);
        zstr_free (&command);
        return 1;
    }
    else
    if (streq(command, "CONNECT"))
    {
	    char *endpoint = zmsg_popstr (message);
		char *name = zmsg_popstr (message);

		if (endpoint && name) {
                    log_debug ("outage_actor: CONNECT: %s/%s", endpoint, name);
		    int rv = mlm_client_connect (self->client, endpoint, 1000, name);
            if (rv == -1)
			    log_error("mlm_client_connect failed\n");
	    }

		zstr_free (&endpoint);
		zstr_free (&name);

    }
    else
    if (streq (command, "CONSUMER"))
    {
        char *stream = zmsg_popstr(message);
        char *regex = zmsg_popstr(message);

        if (stream && regex) {
            log_debug ("CONSUMER: %s/%s", stream, regex);
            int rv = mlm_client_set_consumer (self->client, stream, regex);
            if (rv == -1 )
                log_error("mlm_set_consumer failed");
        }

        zstr_free (&stream);
        zstr_free (&regex);
    }
    else
    if (streq (command, "PRODUCER"))
    {
        char *stream = zmsg_popstr(message);

        if (stream){
            log_debug ("PRODUCER: %s", stream);
            int rv = mlm_client_set_producer (self->client, stream);
            if (rv == -1 )
                log_error ("mlm_client_set_producer");
        }
        zstr_free(&stream);
    }
    else
    if (streq (command, "TIMEOUT"))
    {
        char *timeout = zmsg_popstr(message);

        if (timeout) {
            self->timeout_ms = (uint64_t) atoll (timeout);
            log_debug ("TIMEOUT: \"%s\"/%" PRIu64, timeout, self->timeout_ms);
        }
        zstr_free(&timeout);
    }
    else
    if (streq (command, "ASSET-EXPIRY-SEC"))
    {
        char *timeout = zmsg_popstr(message);

        if (timeout){
            data_set_default_expiry (self->assets, atol (timeout));
            log_debug ("ASSET-EXPIRY-SEC: \"%s\"/%" PRIu64, timeout, atol (timeout));
        }
        zstr_free(&timeout);
    }
    else
    if (streq (command, "STATE-FILE"))
    {
        char *state_file = zmsg_popstr(message);
        if (state_file) {
            self->state_file = strdup (state_file);
            log_debug ("STATE-FILE: %s", state_file);
            int r = s_osrv_load (self);
            if (r != 0)
                log_error ("failed to load state file %s: %m", self->state_file);
        }
        zstr_free(&state_file);
    }
    else
    if (streq (command, "VERBOSE"))
    {
        self->verbose = true;
    }
    else
    if (streq (command, "DEFAULT_MAINTENANCE_EXPIRATION"))
    {
        char *maintenance_expiration = zmsg_popstr(message);
        if (maintenance_expiration) {
            self->default_maintenance_expiration = atoi (maintenance_expiration);
            log_debug ("DEFAULT_MAINTENANCE_EXPIRATION: %s", maintenance_expiration);
        }
        zstr_free(&maintenance_expiration);
    }
    else {
        log_error ("Unknown actor command: %s.\n", command);
    }

    zstr_free (&command);
    zmsg_destroy (message_p);
    return 0;
}

void
metric_processing (fty::shm::shmMetrics& metrics, void* args) {
  
  s_osrv_t *self = (s_osrv_t *) args;

  for (auto &element : metrics) {
    const char *is_computed = fty_proto_aux_string (element, "x-cm-count", NULL);
    if ( !is_computed ) {
        uint64_t now_sec = zclock_time() / 1000;
        uint64_t timestamp = fty_proto_time (element);
        const char* port = fty_proto_aux_string (element, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

        if (port != NULL ) {
            // is it from sensor? yes
            // get sensors attached to the 'asset' on the 'port'! we can have more than 1!
            const char *source = fty_proto_aux_string (element, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
            if (NULL == source) {
                log_error("Sensor message malformed: found %s='%s' but %s is missing", FTY_PROTO_METRICS_SENSOR_AUX_PORT,
                        port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                continue;
            }
            log_debug ("Sensor '%s' on '%s'/'%s' is still alive", source,  fty_proto_name (element), port);
            s_osrv_resolve_alert (self, source);
            int rv = data_touch_asset (self->assets, source, timestamp, fty_proto_ttl (element), now_sec);
            if ( rv == -1 )
                log_error ("asset: name = %s, topic=%s metric is from future! ignore it", source, mlm_client_subject (self->client));
        }
        else {
            // is it from sensor? no
            const char *source = fty_proto_name (element);
            s_osrv_resolve_alert (self, source);
            int rv = data_touch_asset (self->assets, source, timestamp, fty_proto_ttl (element), now_sec);
            if ( rv == -1 )
                log_error ("asset: name = %s, topic=%s metric is from future! ignore it", source, mlm_client_subject (self->client));
        }
    }
    else {
        // intentionally left empty
        // so it is metric from agent-cm -> it is not comming from the device itself ->ignore it
    }
  }
}

void
outage_metric_polling (zsock_t *pipe, void *args)
{
  zpoller_t *poller = zpoller_new (pipe, NULL);
  zsock_signal (pipe, 0);

  while (!zsys_interrupted)
  {
      void *which = zpoller_wait (poller, fty_get_polling_interval() * 1000);
      if (zpoller_terminated(poller) || zsys_interrupted) {
          log_info ("outage_actor: Terminating.");
          break;
      }
      if (zpoller_expired (poller)) {
        fty::shm::shmMetrics result;
        log_debug("read metrics");
        fty::shm::read_metrics(".*", ".*", result);
        log_debug("i have read %d metric", result.size());
        metric_processing(result, args);
      }
      if (which == pipe) {
      zmsg_t *msg = zmsg_recv (pipe);
        if (msg) {
            char *cmd = zmsg_popstr (msg);
            if (cmd) {
                if (streq (cmd, "$TERM")) {
                    zstr_free (&cmd);
                    zmsg_destroy (&msg);
                    break;
                }
                zstr_free (&cmd);
            }
            zmsg_destroy (&msg);
        }
      }
      
  }
  zpoller_destroy(&poller);
}


//  --------------------------------------------------------------------------
//  Handle mailbox messages

static void
fty_outage_handle_mailbox (s_osrv_t *self, zmsg_t **msg)
{
    if (self->verbose)
        zmsg_print (*msg);
    if (msg && *msg) {
        char *message_type = zmsg_popstr(*msg);
        if (!message_type) {
            log_warning("Expected message of type REQUEST");
            return;
        }
        char *zuuid = zmsg_popstr (*msg);
        if (!zuuid) {
            log_warning("Expected zuuid");
            zstr_free (&message_type);
            return;
        }
        char *command = zmsg_popstr (*msg);
        char *sender = strdup (mlm_client_sender (self->client));
        char *subject = strdup (mlm_client_subject (self->client));

        // message model always enforce reply
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr (reply, zuuid);
        zmsg_addstr (reply, "REPLY");

        if (streq (message_type, "REQUEST")) {
            if (!command) {
                log_warning("Expected command");
                zstr_free (&zuuid);
                zstr_free (&message_type);
                zmsg_addstr (reply, "ERROR");
                zmsg_addstr (reply, "Missing command");
            }
            else if (streq (command, "MAINTENANCE_MODE")) {
                // * REQUEST/'msg-correlation-id'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration - switch 'asset1' to 'assetN' into maintenance
                // ex: bmsg request fty-outage GET REQUEST 1234 MAINTENANCE_MODE enable ups-9 3600

                // look for expiration_ttl
                int expiration_ttl = self->default_maintenance_expiration;
                zframe_t *last_frame = zmsg_last (*msg);
                char *last_str = NULL;
                if (last_frame) {
                    last_str = zframe_strdup (last_frame);
                    log_debug("last_str: %s", last_str);
                    // '-' means that it's asset name, otherwise the expiration TTL
                    if (strchr(last_str, '-') == NULL)
                        expiration_ttl = atoi(last_str);
                    zstr_free(&last_str);
                }

                // look for mode 'enable' or 'disable'
                char *mode_str = zmsg_popstr (*msg);
                int mode = ENABLE_MAINTENANCE;

                if (mode_str) {

                    log_debug("Maintenance mode: %s", mode_str);

                    if ( (streq (mode_str, "disable")) || (streq (mode_str, "enable")) ) {
                        if (streq (mode_str, "disable")) {
                            mode = DISABLE_MAINTENANCE;
                            // also restore default TTL
                            expiration_ttl = DEFAULT_ASSET_EXPIRATION_TIME_SEC;
                        }
                        // loop on assets...
                        int rv = -1;
                        char *maint_asset = zmsg_popstr(*msg);
                        while (maint_asset) {
                            // trim potential ttl (last frame)
                            if (strchr(maint_asset, '-') != NULL)
                                rv = s_osrv_maintenance_mode (self, maint_asset, mode, expiration_ttl);

                            zstr_free(&maint_asset);
                            maint_asset = zmsg_popstr(*msg);
                        }
                        // Process result at the end
                        if (rv == 0)
                            zmsg_addstr (reply, "OK");
                        else {
                            zmsg_addstr (reply, "ERROR");
                            zmsg_addstr (reply, "Command failed");
                        }
                    }
                    else {
                        zmsg_addstr (reply, "ERROR");
                        zmsg_addstr (reply, "Unsupported maintenance mode");
                        zstr_free (&mode_str);
                    }
                    zstr_free (&mode_str);
                }
                else {
                    zmsg_addstr (reply, "ERROR");
                    zmsg_addstr (reply, "Missing maintenance mode");
                }

            }
            else {
                // command is not expected
                log_warning ("'%s': invalid command", command);
                zmsg_addstr (reply, "ERROR");
                zmsg_addstr (reply, "Invalid command");
            }
        }
        else {
            // message_type is not expected
            log_warning ("'%s': invalid message type", message_type);
            zmsg_addstr (reply, "ERROR");
            zmsg_addstr (reply, "Invalid message type");
        }

        if (self->verbose)
            zmsg_print (reply);

        mlm_client_sendto (self->client,
                            sender,
                            subject,
                            NULL,
                            5000,
                            &reply);
        if (reply) {
            log_error ("Could not send message to %s", mlm_client_sender (self->client));
            zmsg_destroy (&reply);
        }

        zstr_free (&subject);
        zstr_free (&sender);
        if (command)
            zstr_free (&command);
        zstr_free (&zuuid);
        zstr_free (&message_type);
        zmsg_destroy(msg);
    }
}

// --------------------------------------------------------------------------
// Create a new fty_outage_server
void
fty_outage_server (zsock_t *pipe, void *args)
{
    s_osrv_t *self = s_osrv_new ();
    assert (self);

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (self->client), NULL);
    assert (poller);

    zsock_signal (pipe, 0);
    log_info ("outage_actor: Started");
    //    poller timeout
    uint64_t now_ms = zclock_mono ();
    uint64_t last_dead_check_ms = now_ms;
    uint64_t last_save_ms = now_ms;

    zactor_t *metric_poll = zactor_new(outage_metric_polling, (void*) self);
    while (!zsys_interrupted)
    {
        self->timeout_ms = fty_get_polling_interval() * 1000;
        void *which = zpoller_wait (poller, self->timeout_ms);

        if (which == NULL) {
            if (zpoller_terminated(poller) || zsys_interrupted) {
                log_info ("outage_actor: Terminating.");
                break;
            }
        }

        now_ms = zclock_mono ();

        // save the state
        if ((now_ms - last_save_ms) > SAVE_INTERVAL_MS) {
            int r = s_osrv_save (self);
            if (r != 0)
                log_error ("failed to save state file %s", self->state_file);
            last_save_ms = now_ms;
        }

        // send alerts
        if (zpoller_expired (poller) || (now_ms - last_dead_check_ms) > self->timeout_ms) {
            s_osrv_check_dead_devices (self);
            last_dead_check_ms = zclock_mono ();
        }

        if (which == pipe) {
            log_trace ("which == pipe");
            zmsg_t *msg = zmsg_recv(pipe);
            if (!msg)
                break;

            int rv = s_osrv_actor_commands (self, &msg);
            if (rv == 1)
                break;
            continue;
        }
        // react on incoming messages
        else
        if (which == mlm_client_msgpipe (self->client)) {
            log_trace ("which == mlm_client_msgpipe");

            zmsg_t *message = mlm_client_recv (self->client);
            if (!message)
                break;

            if (!is_fty_proto(message)) {
                if (streq (mlm_client_address (self->client), FTY_PROTO_STREAM_METRICS_UNAVAILABLE)) {
                    char *foo = zmsg_popstr (message);
                    if ( foo && streq (foo, "METRICUNAVAILABLE")) {
                        zstr_free (&foo);
                        foo = zmsg_popstr (message); // topic in form aaaa@bbb
                        const char* source = strstr (foo, "@") + 1;
                        s_osrv_resolve_alert (self, source);
                        data_delete (self->assets, source);
                    }
                    zstr_free (&foo);
                }
                else if (streq (mlm_client_command (self->client), "MAILBOX DELIVER")) {
                    // someone is addressing us directly
                    log_debug("%s: MAILBOX DELIVER", __func__);
                    fty_outage_handle_mailbox(self, &message);
                }
                zmsg_destroy(&message);
                continue;
            }

            fty_proto_t *bmsg = fty_proto_decode (&message);
            if (!bmsg)
                continue;

            // resolve sent alert
            if (fty_proto_id (bmsg) == FTY_PROTO_METRIC || streq (mlm_client_address (self->client), FTY_PROTO_STREAM_METRICS_SENSOR)) {
                const char *is_computed = fty_proto_aux_string (bmsg, "x-cm-count", NULL);
                if ( !is_computed ) {
                    uint64_t now_sec = zclock_time() / 1000;
                    uint64_t timestamp = fty_proto_time (bmsg);
                    const char* port = fty_proto_aux_string (bmsg, FTY_PROTO_METRICS_SENSOR_AUX_PORT, NULL);

                    if (port != NULL ) {
                        // is it from sensor? yes
                        // get sensors attached to the 'asset' on the 'port'! we can have more then 1!
                        const char *source = fty_proto_aux_string (bmsg, FTY_PROTO_METRICS_SENSOR_AUX_SNAME, NULL);
                        if (NULL == source) {
                            log_error("Sensor message malformed: found %s='%s' but %s is missing", FTY_PROTO_METRICS_SENSOR_AUX_PORT,
                                    port, FTY_PROTO_METRICS_SENSOR_AUX_SNAME);
                            continue;
                        }
                        log_debug ("Sensor '%s' on '%s'/'%s' is still alive", source,  fty_proto_name (bmsg), port);
                        s_osrv_resolve_alert (self, source);
                        int rv = data_touch_asset (self->assets, source, timestamp, fty_proto_ttl (bmsg), now_sec);
                        if ( rv == -1 )
                            log_error ("asset: name = %s, topic=%s metric is from future! ignore it", source, mlm_client_subject (self->client));
                    }
                    else {
                        // is it from sensor? no
                        const char *source = fty_proto_name (bmsg);
                        s_osrv_resolve_alert (self, source);
                        int rv = data_touch_asset (self->assets, source, timestamp, fty_proto_ttl (bmsg), now_sec);
                        if ( rv == -1 )
                            log_error ("asset: name = %s, topic=%s metric is from future! ignore it", source, mlm_client_subject (self->client));
                    }
                }
                else {
                    // intentionally left empty
                    // so it is metric from agent-cm -> it is not comming from the device itself ->ignore it
                }
            }
            else
            if (fty_proto_id (bmsg) == FTY_PROTO_ASSET) {
                if (streq (fty_proto_operation (bmsg), FTY_PROTO_ASSET_OP_DELETE)
                     || !streq (fty_proto_aux_string (bmsg, FTY_PROTO_ASSET_STATUS, "active"), "active") )
                {
                    const char* source = fty_proto_name (bmsg);
                    s_osrv_resolve_alert (self, source);
                }
                data_put (self->assets, &bmsg);
            }
            fty_proto_destroy (&bmsg);
        }
    }
    zactor_destroy (&metric_poll);
    zpoller_destroy (&poller);
    int r = s_osrv_save (self);
    if (r != 0)
        log_error ("outage_actor: failed to save state file %s: %m", self->state_file);
    s_osrv_destroy (&self);
    log_info ("outage_actor: Ended");
}

// --------------------------------------------------------------------------
// Self test of this class

void
fty_outage_server_test (bool verbose)
{
    printf (" * fty_outage_server: \n");
    ftylog_setInstance("fty_outage_server_test","");
    if (verbose)
        ftylog_setVeboseMode(ftylog_getInstance());
    //     @selftest
    static const char *endpoint =  "inproc://malamute-test2";

    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND",endpoint, NULL);

    // malamute clients
//    mlm_client_t *m_sender = mlm_client_new();
//    int rv = mlm_client_connect (m_sender, endpoint, 5000, "m_sender");
//    assert (rv >= 0);
//    rv = mlm_client_set_producer (m_sender, "METRICS");
//    assert (rv >= 0);

    int polling_value = 10;
    int wanted_ttl = 2*polling_value-1;
    fty_shm_set_default_polling_interval(polling_value);
    assert(fty_shm_set_test_dir("src/selftest-rw") == 0);

    zactor_t *self = zactor_new (fty_outage_server, (void*) "outage");
    assert (self);

    //    actor commands
    zstr_sendx (self, "CONNECT", endpoint, "fty-outage", NULL);
    zstr_sendx (self, "CONSUMER", "METRICS", ".*", NULL);
    zstr_sendx (self, "CONSUMER", "ASSETS", ".*", NULL);
    zstr_sendx (self, "CONSUMER", "_METRICS_SENSOR", ".*", NULL);
    zstr_sendx (self, "CONSUMER", "_METRICS_UNAVAILABLE", ".*", NULL);
    zstr_sendx (self, "PRODUCER", "_ALERTS_SYS", NULL);
    zstr_sendx (self, "TIMEOUT", "1000", NULL);
    zstr_sendx (self, "ASSET-EXPIRY-SEC", "3", NULL);
    zstr_sendx (server, "DEFAULT_MAINTENANCE_EXPIRATION", "30", NULL);
    if (verbose)
        zstr_sendx (self, "VERBOSE", NULL);

    mlm_client_t *mb_client = mlm_client_new ();
    mlm_client_connect (mb_client, endpoint, 1000, "fty_outage_client");

    mlm_client_t *a_sender = mlm_client_new();
    int rv = mlm_client_connect (a_sender, endpoint, 5000, "a_sender");
    assert (rv >= 0);
    rv = mlm_client_set_producer (a_sender, "ASSETS");
    assert (rv >= 0);

    mlm_client_t *consumer = mlm_client_new();
    rv = mlm_client_connect (consumer, endpoint, 5000, "alert-consumer");
    assert (rv >= 0);
    rv = mlm_client_set_consumer (consumer, "_ALERTS_SYS", ".*");
    assert (rv >= 0);

    //to give a time for all the clients and actors to initialize
    zclock_sleep (1000);

    // test case 01 to send the metric with short TTL
    log_debug ("fty-outage: Test #1");
    zhash_t *asset_ext = zhash_new ();
    zhash_insert (asset_ext, "name", (void *) "ename_of_ups33");
    zhash_t *asset_aux = zhash_new ();
    zhash_insert (asset_aux, "type", (void *) "device");
    zhash_insert (asset_aux, "subtype", (void *) "ups");
    zmsg_t *sendmsg = fty_proto_encode_asset (asset_aux, "UPS33", "create", asset_ext);
    zhash_destroy (&asset_aux);
    zhash_destroy (&asset_ext);

    rv = mlm_client_send (a_sender, "subject",  &sendmsg);

    // expected: ACTIVE alert to be sent
//    sendmsg = fty_proto_encode_metric (
//        NULL,
//        time (NULL),
//        1,
//        "dev",
//        "UPS33",
//        "1",
//        "c");
//
//    rv = mlm_client_send (m_sender, "subject",  &sendmsg);
    rv = fty::shm::write_metric("UPS33", "dev", "1", "c", wanted_ttl);
    assert (rv >= 0);
    zclock_sleep (1000);

    zmsg_t *msg = mlm_client_recv (consumer);
    assert (msg);
    fty_proto_t *bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS33"));
    assert (streq (fty_proto_state (bmsg), "ACTIVE"));
    fty_proto_destroy (&bmsg);

    // test case 02 to resolve alert by sending an another metric
    log_debug ("fty-outage: Test #2");
    // expected: RESOLVED alert to be sent
//    sendmsg = fty_proto_encode_metric (
//        NULL,
//        time (NULL),
//        1000,
//        "dev",
//        "UPS33",
//        "1",
//        "c");
//
//    rv = mlm_client_send (m_sender, "subject",  &sendmsg);
    rv = fty::shm::write_metric("UPS33", "dev", "1", "c", wanted_ttl);
    assert (rv >= 0);

    msg = mlm_client_recv (consumer);
    assert (msg);
    bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS33"));
    assert (streq (fty_proto_state (bmsg), "RESOLVED"));
    fty_proto_destroy (&bmsg);

    //  cleanup from test case 02 - delete asset from cache
    sendmsg = fty_proto_encode_asset (
        NULL,
        "UPS33",
        FTY_PROTO_ASSET_OP_DELETE,
        NULL);
    rv = mlm_client_send (a_sender, "subject",  &sendmsg);
    assert (rv >= 0);

    // test case 03: add new asset device, wait expiry time and check the alert
    log_debug ("fty-outage: Test #3");
    zhash_t *aux = zhash_new ();
    zhash_insert (aux, FTY_PROTO_ASSET_TYPE, (void *) "device");
    zhash_insert (aux, FTY_PROTO_ASSET_SUBTYPE, (void *) "ups");
    zhash_insert (aux, FTY_PROTO_ASSET_STATUS, (void *) "active");
    sendmsg = fty_proto_encode_asset (
        aux,
        "UPS-42",
        FTY_PROTO_ASSET_OP_CREATE,
        NULL);
    zhash_destroy (&aux);
    rv = mlm_client_send (a_sender, "UPS-42",  &sendmsg);
    assert (rv >= 0);

    msg = mlm_client_recv (consumer);
    assert (msg);
    bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS-42"));
    assert (streq (fty_proto_state (bmsg), "ACTIVE"));
    fty_proto_destroy (&bmsg);

    // test case 04: switch the asset device to maintenance mode, and check that
    // 1) alert switches to RESOLVED
    // 2) after TTL, alert is back to active
    // * REQUEST/'msg-correlation-id'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration - switch 'asset1' to 'assetN' into maintenance
    log_debug ("fty-outage: Test #4");
    zmsg_t *request = zmsg_new ();
    zuuid_t *zuuid = zuuid_new ();
    const char *zuuid_str = zuuid_str_canonical (zuuid);
    zmsg_addstr (request, "REQUEST");
    zmsg_addstr (request, zuuid_str);
    zmsg_addstr (request, "MAINTENANCE_MODE");
    zmsg_addstr (request, "enable");
    zmsg_addstr (request, "UPS-42");
    zmsg_addstr (request, "10");

    rv = mlm_client_sendto (mb_client, "fty-outage", "TEST", NULL, 1000, &request);
    assert (rv >= 0);

    // check MB reply
    zmsg_t *recv = mlm_client_recv (mb_client);
    assert (recv);
    char *answer = zmsg_popstr (recv);
    assert (streq (zuuid_str, answer));
    zstr_free(&answer);
    answer = zmsg_popstr (recv);
    assert (streq ("REPLY", answer));
    zstr_free(&answer);
    answer = zmsg_popstr (recv);
    assert (streq ("OK", answer));
    zstr_free(&answer);
    zmsg_destroy (&recv);

    // check ALERT: should be "RESOLVED" since the asset is in maintenance mode
    msg = mlm_client_recv (consumer);
    bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS-42"));
    assert (streq (fty_proto_state (bmsg), "RESOLVED"));
    fty_proto_destroy (&bmsg);

    // wait a bit before checking for (2)
    zclock_sleep (1000);

    // check ALERT: should be "ACTIVE" again since the asset has been auto
    // expelled from maintenance mode
    msg = mlm_client_recv (consumer);
    assert (msg);
    bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS-42"));
    assert (streq (fty_proto_state (bmsg), "ACTIVE"));
    fty_proto_destroy (&bmsg);
    zuuid_destroy (&zuuid);

    // test case 05: RESOLVE alert when device is retired
    log_debug ("fty-outage: Test #5");
    aux = zhash_new ();
    zhash_insert (aux, FTY_PROTO_ASSET_TYPE, (void *) "device");
    zhash_insert (aux, FTY_PROTO_ASSET_SUBTYPE, (void *) "ups");
    zhash_insert (aux, FTY_PROTO_ASSET_STATUS, (void *) "retired");
    sendmsg = fty_proto_encode_asset (
        aux,
        "UPS-42",
        FTY_PROTO_ASSET_OP_UPDATE,
        NULL);
    zhash_destroy (&aux);
    rv = mlm_client_send (a_sender, "UPS-42",  &sendmsg);
    assert (rv >= 0);

    msg = mlm_client_recv (consumer);
    assert (msg);
    bmsg = fty_proto_decode (&msg);
    assert (bmsg);
    if (verbose)
        fty_proto_print (bmsg);
    assert (streq (fty_proto_name (bmsg), "UPS-42"));
    assert (streq (fty_proto_state (bmsg), "RESOLVED"));
    fty_proto_destroy (&bmsg);
    zactor_destroy(&self);
//    mlm_client_destroy (&m_sender);
    fty_shm_delete_test_dir();
    mlm_client_destroy (&a_sender);
    mlm_client_destroy (&consumer);
    mlm_client_destroy (&mb_client);
    zactor_destroy (&server);

    //  @end

    // Those are PRIVATE to actor, so won't be a part of documentation
    s_osrv_t * self2 = s_osrv_new ();
    zhash_insert (self2->active_alerts, "DEVICE1", TRUE);
    zhash_insert (self2->active_alerts, "DEVICE2", TRUE);
    zhash_insert (self2->active_alerts, "DEVICE3", TRUE);
    zhash_insert (self2->active_alerts, "DEVICE WITH SPACE", TRUE);
    self2->state_file = strdup ("src/state.zpl");
    s_osrv_save (self2);
    s_osrv_destroy (&self2);

    self2 = s_osrv_new ();
    self2->state_file = strdup ("src/state.zpl");
    s_osrv_load (self2);

    assert (zhash_size (self2->active_alerts) == 4);
    assert (zhash_lookup (self2->active_alerts, "DEVICE1"));
    assert (zhash_lookup (self2->active_alerts, "DEVICE2"));
    assert (zhash_lookup (self2->active_alerts, "DEVICE3"));
    assert (zhash_lookup (self2->active_alerts, "DEVICE WITH SPACE"));
    assert (!zhash_lookup (self2->active_alerts, "DEVICE4"));

    s_osrv_destroy (&self2);

    unlink ("src/state.zpl");
    printf ("OK\n");
}
