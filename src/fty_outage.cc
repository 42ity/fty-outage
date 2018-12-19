/*
    fty_agent_outage - Agent outage

    Copyright (C) 2014 - 2017 Eaton                                        
                                                                           
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
    fty_agent_outage - Agent outage
@discuss
@end
*/

#include "fty_outage_classes.h"

static const char *CONFIG = "/etc/fty-outage/fty-outage.cfg";

int main (int argc, char *argv [])
{
    const char * logConfigFile = "";
    ftylog_setInstance("fty-outage","");
    bool verbose = false;
    int argn;
    for (argn = 1; argn < argc; argn++) {
        if (streq (argv [argn], "--help")
        ||  streq (argv [argn], "-h")) {
            puts ("fty-outage [options] ...");
            puts ("  --verbose / -v         verbose test output");
            puts ("  --help / -h            this information");
            return 0;
        }
        else
        if (streq (argv [argn], "--verbose")
        ||  streq (argv [argn], "-v"))
            verbose = true;
        else {
            printf ("Unknown option: %s\n", argv [argn]);
        }
    }
    
    zconfig_t *cfg = zconfig_load(CONFIG);
    log_debug("Config is %s null",cfg ? "not": "");
    if (cfg) {
        logConfigFile = zconfig_get(cfg, "log/config", "");
    }
    //If a log config file is configured, try to load it
    if (!streq(logConfigFile,""))
    {
      log_debug("Try to load clog configuration file : %s",logConfigFile);
      ftylog_setConfigFile(ftylog_getInstance(),logConfigFile);
    }
    
    if (verbose)
    {
        ftylog_setVeboseMode(ftylog_getInstance());
    }
    
    zactor_t *server = zactor_new (fty_outage_server, (void *) "outage");
    //  Insert main code here
    
    zstr_sendx (server, "STATE-FILE", "/var/lib/fty/fty-outage/state.zpl", NULL);
    zstr_sendx (server, "TIMEOUT", "30000", NULL);
    zstr_sendx (server, "CONNECT", "ipc://@/malamute", "fty-outage", NULL);
    zstr_sendx (server, "PRODUCER", FTY_PROTO_STREAM_ALERTS_SYS, NULL);
    //zstr_sendx (server, "CONSUMER", FTY_PROTO_STREAM_METRICS, ".*", NULL);
    zstr_sendx (server, "CONSUMER", FTY_PROTO_STREAM_METRICS_UNAVAILABLE, ".*", NULL);
    zstr_sendx (server, "CONSUMER", FTY_PROTO_STREAM_METRICS_SENSOR, ".*", NULL);
    zstr_sendx (server, "CONSUMER", FTY_PROTO_STREAM_ASSETS, ".*", NULL);

    // src/malamute.c, under MPL license
    while (true) {
        char *str = zstr_recv (server);
        if (str) {
            puts (str);
            zstr_free (&str);
        }
        else {
            log_info ("Interrupted ...");
            break;
        }
    }
    zactor_destroy (&server);
    return 0;
}