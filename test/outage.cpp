#include <catch2/catch.hpp>
#include <malamute.h>
#include <fty_shm.h>
#include "src/fty-outage-server.h"
#include "src/data.h"
#include "src/osrv.h"

TEST_CASE("outage server test")
{
    static const char* endpoint = "inproc://malamute-test2";

    zactor_t* server = zactor_new(mlm_server, const_cast<char*>("Malamute"));
    zstr_sendx(server, "BIND", endpoint, NULL);

    // malamute clients
    //    mlm_client_t *m_sender = mlm_client_new();
    //    int rv = mlm_client_connect (m_sender, endpoint, 5000, "m_sender");
    //    assert (rv >= 0);
    //    rv = mlm_client_set_producer (m_sender, "METRICS");
    //    assert (rv >= 0);

    int polling_value = 10;
    int wanted_ttl    = 2 * polling_value - 1;
    fty_shm_set_default_polling_interval(polling_value);
    CHECK(fty_shm_set_test_dir(".") == 0);

    zactor_t* self = zactor_new(fty_outage_server, const_cast<char*>("outage"));
    REQUIRE(self);

    //    actor commands
    zstr_sendx(self, "CONNECT", endpoint, "fty-outage", NULL);
    zstr_sendx(self, "CONSUMER", "METRICS", ".*", NULL);
    zstr_sendx(self, "CONSUMER", "ASSETS", ".*", NULL);
    zstr_sendx(self, "CONSUMER", "_METRICS_SENSOR", ".*", NULL);
    zstr_sendx(self, "CONSUMER", "_METRICS_UNAVAILABLE", ".*", NULL);
    zstr_sendx(self, "PRODUCER", "_ALERTS_SYS", NULL);
    zstr_sendx(self, "TIMEOUT", "1000", NULL);
    zstr_sendx(self, "ASSET-EXPIRY-SEC", "3", NULL);
    zstr_sendx(server, "DEFAULT_MAINTENANCE_EXPIRATION", "30", NULL);

    mlm_client_t* mb_client = mlm_client_new();
    mlm_client_connect(mb_client, endpoint, 1000, "fty_outage_client");

    mlm_client_t* a_sender = mlm_client_new();
    int           rv       = mlm_client_connect(a_sender, endpoint, 5000, "a_sender");
    assert(rv >= 0);
    rv = mlm_client_set_producer(a_sender, "ASSETS");
    assert(rv >= 0);

    mlm_client_t* consumer = mlm_client_new();
    rv                     = mlm_client_connect(consumer, endpoint, 5000, "alert-consumer");
    assert(rv >= 0);
    rv = mlm_client_set_consumer(consumer, "_ALERTS_SYS", ".*");
    assert(rv >= 0);

    // to give a time for all the clients and actors to initialize
    zclock_sleep(1000);

    // test case 01 to send the metric with short TTL
    zhash_t* asset_ext = zhash_new();
    zhash_insert(asset_ext, "name", const_cast<char*>("ename_of_ups33"));
    zhash_t* asset_aux = zhash_new();
    zhash_insert(asset_aux, "type", const_cast<char*>("device"));
    zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
    zmsg_t* sendmsg = fty_proto_encode_asset(asset_aux, "UPS33", "create", asset_ext);
    zhash_destroy(&asset_aux);
    zhash_destroy(&asset_ext);

    rv = mlm_client_send(a_sender, "subject", &sendmsg);
    REQUIRE(rv >= 0);

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
    REQUIRE(rv >= 0);
    zclock_sleep(1000);

    zmsg_t* msg = mlm_client_recv(consumer);
    REQUIRE(msg);
    fty_proto_t* bmsg = fty_proto_decode(&msg);
    REQUIRE(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS33"));
    CHECK(streq(fty_proto_state(bmsg), "ACTIVE"));
    fty_proto_destroy(&bmsg);

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
    REQUIRE(rv >= 0);

    msg = mlm_client_recv(consumer);
    REQUIRE(msg);
    bmsg = fty_proto_decode(&msg);
    REQUIRE(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS33"));
    CHECK(streq(fty_proto_state(bmsg), "RESOLVED"));
    fty_proto_destroy(&bmsg);

    //  cleanup from test case 02 - delete asset from cache
    sendmsg = fty_proto_encode_asset(NULL, "UPS33", FTY_PROTO_ASSET_OP_DELETE, NULL);
    rv      = mlm_client_send(a_sender, "subject", &sendmsg);
    REQUIRE(rv >= 0);

    // test case 03: add new asset device, wait expiry time and check the alert
    zhash_t* aux = zhash_new();
    zhash_insert(aux, FTY_PROTO_ASSET_TYPE, const_cast<char*>("device"));
    zhash_insert(aux, FTY_PROTO_ASSET_SUBTYPE, const_cast<char*>("ups"));
    zhash_insert(aux, FTY_PROTO_ASSET_STATUS, const_cast<char*>("active"));
    sendmsg = fty_proto_encode_asset(aux, "UPS-42", FTY_PROTO_ASSET_OP_CREATE, NULL);
    zhash_destroy(&aux);
    rv = mlm_client_send(a_sender, "UPS-42", &sendmsg);
    REQUIRE(rv >= 0);

    msg = mlm_client_recv(consumer);
    REQUIRE(msg);
    bmsg = fty_proto_decode(&msg);
    CHECK(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS-42"));
    CHECK(streq(fty_proto_state(bmsg), "ACTIVE"));
    fty_proto_destroy(&bmsg);

    // test case 04: switch the asset device to maintenance mode, and check that
    // 1) alert switches to RESOLVED
    // 2) after TTL, alert is back to active
    // * REQUEST/'msg-correlation-id'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration - switch 'asset1' to 'assetN'
    // into maintenance
    zmsg_t*     request   = zmsg_new();
    zuuid_t*    zuuid     = zuuid_new();
    const char* zuuid_str = zuuid_str_canonical(zuuid);
    zmsg_addstr(request, "REQUEST");
    zmsg_addstr(request, zuuid_str);
    zmsg_addstr(request, "MAINTENANCE_MODE");
    zmsg_addstr(request, "enable");
    zmsg_addstr(request, "UPS-42");
    zmsg_addstr(request, "10");

    rv = mlm_client_sendto(mb_client, "fty-outage", "TEST", NULL, 1000, &request);
    REQUIRE(rv >= 0);

    // check MB reply
    zmsg_t* recv = mlm_client_recv(mb_client);
    REQUIRE(recv);
    char* answer = zmsg_popstr(recv);
    CHECK(streq(zuuid_str, answer));
    zstr_free(&answer);
    answer = zmsg_popstr(recv);
    CHECK(streq("REPLY", answer));
    zstr_free(&answer);
    answer = zmsg_popstr(recv);
    CHECK(streq("OK", answer));
    zstr_free(&answer);
    zmsg_destroy(&recv);

    // check ALERT: should be "RESOLVED" since the asset is in maintenance mode
    msg  = mlm_client_recv(consumer);
    bmsg = fty_proto_decode(&msg);
    REQUIRE(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS-42"));
    CHECK(streq(fty_proto_state(bmsg), "RESOLVED"));
    fty_proto_destroy(&bmsg);

    // wait a bit before checking for (2)
    zclock_sleep(1000);

    // check ALERT: should be "ACTIVE" again since the asset has been auto
    // expelled from maintenance mode
    msg = mlm_client_recv(consumer);
    REQUIRE(msg);
    bmsg = fty_proto_decode(&msg);
    REQUIRE(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS-42"));
    CHECK(streq(fty_proto_state(bmsg), "ACTIVE"));
    fty_proto_destroy(&bmsg);
    zuuid_destroy(&zuuid);

    // test case 05: RESOLVE alert when device is retired
    aux = zhash_new();
    zhash_insert(aux, FTY_PROTO_ASSET_TYPE, const_cast<char*>("device"));
    zhash_insert(aux, FTY_PROTO_ASSET_SUBTYPE, const_cast<char*>("ups"));
    zhash_insert(aux, FTY_PROTO_ASSET_STATUS, const_cast<char*>("retired"));
    sendmsg = fty_proto_encode_asset(aux, "UPS-42", FTY_PROTO_ASSET_OP_UPDATE, NULL);
    zhash_destroy(&aux);
    rv = mlm_client_send(a_sender, "UPS-42", &sendmsg);
    REQUIRE(rv >= 0);

    msg = mlm_client_recv(consumer);
    REQUIRE(msg);
    bmsg = fty_proto_decode(&msg);
    REQUIRE(bmsg);
    CHECK(streq(fty_proto_name(bmsg), "UPS-42"));
    CHECK(streq(fty_proto_state(bmsg), "RESOLVED"));
    fty_proto_destroy(&bmsg);
    zactor_destroy(&self);
    //    mlm_client_destroy (&m_sender);
    fty_shm_delete_test_dir();
    mlm_client_destroy(&a_sender);
    mlm_client_destroy(&consumer);
    mlm_client_destroy(&mb_client);
    zactor_destroy(&server);

    //  @end

    // Those are PRIVATE to actor, so won't be a part of documentation
    s_osrv_t* self2 = s_osrv_new();
    zhash_insert(self2->active_alerts, "DEVICE1", TRUE);
    zhash_insert(self2->active_alerts, "DEVICE2", TRUE);
    zhash_insert(self2->active_alerts, "DEVICE3", TRUE);
    zhash_insert(self2->active_alerts, "DEVICE WITH SPACE", TRUE);
    self2->state_file = strdup("state.zpl");
    s_osrv_save(self2);
    s_osrv_destroy(&self2);

    self2             = s_osrv_new();
    self2->state_file = strdup("state.zpl");
    s_osrv_load(self2);

    REQUIRE(zhash_size(self2->active_alerts) == 4);
    CHECK(zhash_lookup(self2->active_alerts, "DEVICE1"));
    CHECK(zhash_lookup(self2->active_alerts, "DEVICE2"));
    CHECK(zhash_lookup(self2->active_alerts, "DEVICE3"));
    CHECK(zhash_lookup(self2->active_alerts, "DEVICE WITH SPACE"));
    CHECK(!zhash_lookup(self2->active_alerts, "DEVICE4"));

    s_osrv_destroy(&self2);

    unlink("state.zpl");
}
