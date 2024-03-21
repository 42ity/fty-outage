/*
    fty_agent_outage - Agent outage

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

#include <fty_common_agents.h>
#include <fty_common_mlm_utils.h>
#include <fty_log.h>
#include <fty_proto.h>

// default maintenance mode time (seconds)
#define DEFAULT_MAINTENANCE_EXPIRATION "3600"

static void usage()
{
    printf("%s [options] ...\n", AGENT_FTY_OUTAGE);
    printf("  -v/--verbose        verbose test output\n");
    printf("  -h/--help           this information\n");
    printf("  -c/--config <path>  path to config file\n");
}

int main(int argc, char* argv[])
{
    const char* config_file = "/etc/fty-outage/fty-outage.cfg";
    const char* state_file = "/var/lib/fty/fty-outage/state.zpl";
    const char* maintenance_expiration = DEFAULT_MAINTENANCE_EXPIRATION;
    bool verbose = false;

    // Parse command line
    for (int argn = 1; argn < argc; argn++) {
        char* param = (argn < argc - 1) ? argv[argn + 1] : NULL;

        if (streq(argv[argn], "--help") || streq(argv[argn], "-h")) {
            usage();
            return EXIT_SUCCESS;
        }
        else if (streq(argv[argn], "--verbose") || streq(argv[argn], "-v")) {
            verbose = true;
        }
        else if (streq(argv[argn], "--config") || streq(argv[argn], "-c")) {
            if (!param) {
                fprintf(stderr, "%s: Missing argument\n", argv[argn]);
                usage();
                return EXIT_FAILURE;
            }
            config_file = param;
            ++argn;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[argn]);
        }
    }

    ftylog_setInstance(AGENT_FTY_OUTAGE, FTY_COMMON_LOGGING_DEFAULT_CFG);

    if (verbose) {
        ftylog_setVerboseMode(ftylog_getInstance());
    }

    zconfig_t* cfg = zconfig_load(config_file);
    if (cfg) {
        // Get maintenance mode expiry (default)
        maintenance_expiration = zconfig_get(cfg, "server/maintenance_expiration", DEFAULT_MAINTENANCE_EXPIRATION);
    }

    zactor_t* server = zactor_new(fty_outage_server, const_cast<char*>(AGENT_FTY_OUTAGE));
    if (!server) {
        logError("{} actor creation failed", AGENT_FTY_OUTAGE);
        zconfig_destroy(&cfg);
        return EXIT_FAILURE;
    }

    zstr_sendx(server, "STATE_FILE", state_file, NULL);
    zstr_sendx(server, "CONNECT", MLM_ENDPOINT, AGENT_FTY_OUTAGE, NULL);
    zstr_sendx(server, "PRODUCER", FTY_PROTO_STREAM_ALERTS_SYS, NULL);
    zstr_sendx(server, "CONSUMER", FTY_PROTO_STREAM_METRICS_UNAVAILABLE, ".*", NULL);
    zstr_sendx(server, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);
    zstr_sendx(server, "DEFAULT_MAINTENANCE_EXPIRATION_SEC", maintenance_expiration, NULL);
    if (verbose) {
        zstr_send(server, "VERBOSE");
    }

    // src/malamute.c, under MPL license
    while (!zsys_interrupted) {
        char* str = zstr_recv(server);
        if (!str) {
            break; //$TERM
        }
        logDebug("{}", str);
        zstr_free(&str);
    }

    zactor_destroy(&server);
    zconfig_destroy(&cfg);

    return EXIT_SUCCESS;
}
