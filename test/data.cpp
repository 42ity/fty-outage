#include <catch2/catch.hpp>
#include "src/data.h"
#include "src/expiration.h"

TEST_CASE("data test0")
{
    data_t* data = data_new();
    CHECK(data);
    data_destroy(&data);
    CHECK(!data);
}

static uint64_t zhashx_get_expiration_test(data_t* self, char* source)
{
    REQUIRE(self);
    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(data_asset_expir(self), source));
    return expiration_time(e);
}

TEST_CASE("data test1")
{
    //  aux data for metric - var_name | msg issued
    zhash_t* aux = zhash_new();
    zhash_update(aux, "key1", const_cast<char*>("val1"));
    zhash_update(aux, "time", const_cast<char*>("2"));
    zhash_update(aux, "key2", const_cast<char*>("val2"));

    // key | expiration (t+2*ttl)
    data_t* data = data_new();
    REQUIRE(data);

    // the default inlined in data.cc
    const uint64_t DEFAULT_ASSET_EXPIRATION_TIME_SEC = ((15 * 60) / 2);

    // get/set test
    CHECK(data_default_expiry(data) == DEFAULT_ASSET_EXPIRATION_TIME_SEC);
    data_set_default_expiry(data, 42);
    CHECK(data_default_expiry(data) == 42);
    data_set_default_expiry(data, 2);

    // data_put UPS4 UPS3
    {
        zhash_t* asset_aux = zhash_new();
        zhash_insert(asset_aux, "type", const_cast<char*>("device"));
        zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
        zmsg_t*      asset   = fty_proto_encode_asset(asset_aux, "UPS4", "create", NULL);
        fty_proto_t* proto_n = fty_proto_decode(&asset);
        CHECK(proto_n);
        data_put(data, &proto_n);
        CHECK(proto_n == NULL);
        zhash_destroy(&asset_aux);
        fty_proto_destroy(&proto_n);
    }
    {
        zhash_t* asset_aux = zhash_new();
        zhash_insert(asset_aux, "type", const_cast<char*>("device"));
        zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
        zmsg_t*      asset   = fty_proto_encode_asset(asset_aux, "UPS3", "create", NULL);
        fty_proto_t* proto_n = fty_proto_decode(&asset);
        CHECK(proto_n);
        data_put(data, &proto_n);
        CHECK(proto_n == NULL);
        zhash_destroy(&asset_aux);
        fty_proto_destroy(&proto_n);
    }

    // create new metric UPS4 - exp NOK
    uint64_t now_sec = uint64_t(zclock_time() / 1000);
    int      rv      = data_touch_asset(data, "UPS4", now_sec, 3, now_sec);
    REQUIRE(rv >= 0);

    // create new metric UPS3 - exp NOT OK
    now_sec = uint64_t(zclock_time() / 1000);
    rv      = data_touch_asset(data, "UPS3", now_sec, 1, now_sec);
    REQUIRE(rv >= 0);

    zclock_sleep(5000);

    // give me dead devices
    auto list = data_get_dead_devices(data);
    REQUIRE(list.size() == 2);

    // update metric - exp OK
    now_sec = uint64_t(zclock_time() / 1000);
    rv      = data_touch_asset(data, "UPS4", now_sec, 2, now_sec);
    REQUIRE(rv == 0);

    // give me dead devices
    list = data_get_dead_devices(data);
    REQUIRE(list.size() == 1);

    // test asset message
    zhash_destroy(&aux);
    zhash_t* ext = zhash_new();
    zhash_insert(ext, "name", const_cast<char*>("ename_of_pdu1"));
    aux = zhash_new();
    zhash_insert(aux, "status", const_cast<char*>("active"));
    zhash_insert(aux, "type", const_cast<char*>("device"));
    zhash_insert(aux, FTY_PROTO_ASSET_SUBTYPE, const_cast<char*>("epdu"));
    zmsg_t*      msg  = fty_proto_encode_asset(aux, "PDU1", FTY_PROTO_ASSET_OP_CREATE, ext);
    fty_proto_t* bmsg = fty_proto_decode(&msg);
    data_put(data, &bmsg);
    CHECK(bmsg == NULL);
    fty_proto_destroy(&bmsg);

    CHECK(zhashx_lookup(data_asset_expir(data), "PDU1"));
    now_sec       = uint64_t(zclock_time() / 1000);
    uint64_t diff = zhashx_get_expiration_test(data, const_cast<char*>("PDU1")) - now_sec;
    CHECK(diff <= (data_default_expiry(data) * 2));
    // TODO: test it more

    CHECK(streq(data_get_asset_ename(data, "PDU1"), "ename_of_pdu1"));

    zhash_destroy(&aux);
    zhash_destroy(&ext);
    data_destroy(&data);
}
