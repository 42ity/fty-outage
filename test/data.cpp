#include "src/data.h"
#include <catch2/catch.hpp>

static void test0()
{
    data_t* data = data_new();
    data_destroy(&data);
}

static void test2()
{
    fty_proto_t*  msg = fty_proto_new(FTY_PROTO_ASSET);
    expiration_t* e   = expiration_new(10, &msg);

    expiration_destroy(&e);
}

static void test3()
{
    fty_proto_t*  msg = fty_proto_new(FTY_PROTO_ASSET);
    expiration_t* e   = expiration_new(10, &msg);
    zclock_sleep(1000);

    uint64_t old_last_seen_date = e->last_time_seen_sec;
    expiration_update(e, uint64_t(zclock_time() / 1000));
    CHECK(e->last_time_seen_sec != old_last_seen_date);

    // from past!!
    old_last_seen_date = e->last_time_seen_sec;
    expiration_update(e, uint64_t(zclock_time() / 1000 - 10000));
    CHECK(e->last_time_seen_sec == old_last_seen_date);

    expiration_update_ttl(e, 1);
    CHECK(e->ttl_sec == 1);

    expiration_update_ttl(e, 10);
    CHECK(e->ttl_sec == 1); // because 10 > 1

    CHECK(expiration_get(e) == old_last_seen_date + 1 * 2);
    expiration_destroy(&e);
}

uint64_t zhashx_get_expiration_test(data_t* self, char* source)
{
    REQUIRE(self);
    expiration_t* e = reinterpret_cast<expiration_t*>(zhashx_lookup(self->assets, source));
    return expiration_get(e);
}

TEST_CASE("data test")
{
    test0();
    test2();
    test3();

    //  aux data for metric - var_name | msg issued
    zhash_t* aux = zhash_new();

    zhash_update(aux, "key1", const_cast<char*>("val1"));
    zhash_update(aux, "time", const_cast<char*>("2"));
    zhash_update(aux, "key2", const_cast<char*>("val2"));

    // key | expiration (t+2*ttl)
    data_t* data = data_new();
    REQUIRE(data);

    // get/set test
    CHECK(data_default_expiry(data) == DEFAULT_ASSET_EXPIRATION_TIME_SEC);
    data_set_default_expiry(data, 42);
    CHECK(data_default_expiry(data) == 42);
    data_set_default_expiry(data, 2);

    // create asset first
    zhash_t* asset_aux = zhash_new();
    zhash_insert(asset_aux, "type", const_cast<char*>("device"));
    zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
    zmsg_t*      asset   = fty_proto_encode_asset(asset_aux, "UPS4", "create", NULL);
    fty_proto_t* proto_n = fty_proto_decode(&asset);
    data_put(data, &proto_n);
    zhash_destroy(&asset_aux);

    asset_aux = zhash_new();
    zhash_insert(asset_aux, "type", const_cast<char*>("device"));
    zhash_insert(asset_aux, "subtype", const_cast<char*>("ups"));
    asset   = fty_proto_encode_asset(asset_aux, "UPS3", "create", NULL);
    proto_n = fty_proto_decode(&asset);
    data_put(data, &proto_n);
    zhash_destroy(&asset_aux);

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
    auto list = data_get_dead(data);
    REQUIRE(list.size() == 2);

    // zlistx_destroy(&list);

    // update metric - exp OK
    now_sec = uint64_t(zclock_time() / 1000);
    rv      = data_touch_asset(data, "UPS4", now_sec, 2, now_sec);
    REQUIRE(rv == 0);

    // give me dead devices
    list = data_get_dead(data);
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

    CHECK(zhashx_lookup(data->assets, "PDU1"));
    now_sec       = uint64_t(zclock_time() / 1000);
    uint64_t diff = zhashx_get_expiration_test(data, const_cast<char*>("PDU1")) - now_sec;
    CHECK(diff <= (data_default_expiry(data) * 2));
    // TODO: test it more

    CHECK(streq(data_get_asset_ename(data, "PDU1"), "ename_of_pdu1"));

    // zlistx_destroy(&list);
    fty_proto_destroy(&proto_n);
    zhash_destroy(&aux);
    zhash_destroy(&ext);
    data_destroy(&data);
}
