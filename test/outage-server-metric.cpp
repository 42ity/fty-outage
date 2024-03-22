#include <catch2/catch.hpp>
#include "src/fty-outage-server.h"
#include "src/outage-metric.h"
#include <malamute.h>
#include <fty_log.h>
#include <fty_shm.h>

//
static void print_metrics()
{
    fty::shm::shmMetrics metrics;
    fty::shm::read_metrics(".*", ".*", metrics);

    log_debug("== metrics (size: %lu)", metrics.size());
    int cpt = 0;
    for (auto& metric : metrics) {
        log_debug("== %d: %s@%s/%s (ttl=%lus)", cpt++,
            fty_proto_type(metric), fty_proto_name(metric), fty_proto_value(metric), fty_proto_ttl(metric));
    }
}

//
static std::string outage_metric_value(const std::string& asset)
{
    std::string ret;
    int r = fty::shm::read_metric_value(asset, "outage", ret);
    return (r == 0) ? ret : "failed";
}

TEST_CASE("outage server metric shm test")
{
    REQUIRE(fty_shm_set_test_dir("./shm2x") == 0);

    using namespace fty::shm;

    CHECK(0 == outage::write("asset0", outage::Status::UNKNOWN, 1000, 0));
    CHECK(0 == outage::write("asset1", outage::Status::INACTIVE, 1000, 0));
    CHECK(0 == outage::write("asset2", outage::Status::ACTIVE, 1000, 0));

    print_metrics();

    CHECK(outage_metric_value("asset0") == "UNKNOWN");
    CHECK(outage_metric_value("asset1") == "INACTIVE");
    CHECK(outage_metric_value("asset2") == "ACTIVE");

    fty_shm_delete_test_dir();
}

TEST_CASE("outage server metric test")
{
    const char* outage_server_address = "fty-outage-test";
    const char* endpoint = "inproc://malamute-fty-outage-test";

    zactor_t* server = zactor_new(mlm_server, const_cast<char*>("Malamute"));
    REQUIRE(server);
    zstr_sendx(server, "BIND", endpoint, NULL);

    int polling_value = 10;
    int ttl_2 = (2 * polling_value) - 1;

    fty_shm_set_default_polling_interval(polling_value);
    REQUIRE(fty_shm_set_test_dir("./shm2y") == 0);

    // outage actor
    zactor_t* outage_actor = zactor_new(fty_outage_server, const_cast<char*>(outage_server_address));
    REQUIRE(outage_actor);

    // actor commands
    zstr_sendx(outage_actor, "CONNECT", endpoint, outage_server_address, NULL);
    zstr_sendx(outage_actor, "CONSUMER", "ASSETS", ".*", NULL);
    zstr_sendx(outage_actor, "VERBOSE", NULL);

    mlm_client_t* asset_producer = mlm_client_new();
    REQUIRE(asset_producer);
    int rv = mlm_client_connect(asset_producer, endpoint, 5000, "asset-producer");
    CHECK(rv >= 0);
    rv = mlm_client_set_producer(asset_producer, "ASSETS");
    CHECK(rv >= 0);

    // create asset UPS33
    zmsg_t* sendmsg = NULL;
    {
        zhash_t* asset_ext = zhash_new();
        zhash_insert(asset_ext, "name", const_cast<char*>("ename_of_ups"));
        zhash_t* asset_aux = zhash_new();
        zhash_insert(asset_aux, "type", const_cast<char*>("device"));
        zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
        sendmsg = fty_proto_encode_asset(asset_aux, "UPS33", "create", asset_ext);
        zhash_destroy(&asset_aux);
        zhash_destroy(&asset_ext);
    }
    rv = mlm_client_send(asset_producer, "UPS33", &sendmsg);
    REQUIRE(rv >= 0);

    // create asset EPDU44
    sendmsg = NULL;
    {
        zhash_t* asset_ext = zhash_new();
        zhash_insert(asset_ext, "name", const_cast<char*>("ename_of_epdu"));
        zhash_t* asset_aux = zhash_new();
        zhash_insert(asset_aux, "type", const_cast<char*>("device"));
        zhash_insert(asset_aux, "subtype", const_cast<char*>("epdu"));
        sendmsg = fty_proto_encode_asset(asset_aux, "EPDU44", "create", asset_ext);
        zhash_destroy(&asset_aux);
        zhash_destroy(&asset_ext);
    }
    rv = mlm_client_send(asset_producer, "EPDU44", &sendmsg);
    REQUIRE(rv >= 0);

    zclock_sleep(1000); // sync

    // outage metrics are in unknown state (not polled yet)
    print_metrics();
    CHECK(outage_metric_value("UPS33") == "UNKNOWN");
    CHECK(outage_metric_value("EPDU44") == "UNKNOWN");

    // wait poll sync
    zclock_sleep((polling_value + 1) * 1000);

    // outage metrics are in active state (UPS/EPDU are down)
    print_metrics();
    CHECK(outage_metric_value("UPS33") == "ACTIVE");
    CHECK(outage_metric_value("EPDU44") == "ACTIVE");

    // populate UPS metric
    rv = fty::shm::write_metric("UPS33", "dev", "1", "c", ttl_2);
    REQUIRE(rv >= 0);

    // wait poll sync
    zclock_sleep((polling_value + 1) * 1000);

    // UPS is up, EPDU is down
    print_metrics();
    CHECK(outage_metric_value("UPS33") == "INACTIVE");
    CHECK(outage_metric_value("EPDU44") == "ACTIVE");

    // populate UPS/EPDU metrics
    rv = fty::shm::write_metric("UPS33", "dev", "1", "c", ttl_2);
    REQUIRE(rv >= 0);
    rv = fty::shm::write_metric("EPDU44", "dev", "1", "c", ttl_2);
    REQUIRE(rv >= 0);

    // wait poll sync
    zclock_sleep((polling_value + 1) * 1000);

    // UPS/EPDU are up
    print_metrics();
    CHECK(outage_metric_value("UPS33") == "INACTIVE");
    CHECK(outage_metric_value("EPDU44") == "INACTIVE");

    // delete asset UPS33
    sendmsg = NULL;
    {
        zhash_t* asset_aux = zhash_new();
        zhash_insert(asset_aux, "type", const_cast<char*>("device"));
        zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
        sendmsg = fty_proto_encode_asset(asset_aux, "UPS33", "delete", NULL);
        zhash_destroy(&asset_aux);
    }
    rv = mlm_client_send(asset_producer, "UPS33", &sendmsg);
    REQUIRE(rv >= 0);

    // unpopulate EPDU44 (short ttl)
    rv = fty::shm::write_metric("EPDU44", "dev", "1", "c", polling_value / 2);
    REQUIRE(rv >= 0);

    // wait poll sync
    zclock_sleep((polling_value + 1) * 1000);

    // UPS is deleted / EPDU is down
    print_metrics();
    CHECK(outage_metric_value("UPS33") == "UNKNOWN");
    CHECK(outage_metric_value("EPDU44") == "ACTIVE"); // metric is gone

    // done, cleanup
    mlm_client_destroy(&asset_producer);
    zactor_destroy(&outage_actor);
    zactor_destroy(&server);
    fty_shm_delete_test_dir();
}
