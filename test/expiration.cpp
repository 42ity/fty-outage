#include <catch2/catch.hpp>
#include "src/expiration.h"
#include <czmq.h>

TEST_CASE("expiration test0")
{
    expiration_t* e = expiration_new(10);
    CHECK(e);
    CHECK(expiration_ttl(e) == 10);
    CHECK(expiration_last_time_seen(e) == 0);
    CHECK(expiration_maintenance(e) == 0);
    expiration_destroy(&e);
    CHECK(!e);
}

TEST_CASE("expiration test1")
{
    expiration_t* e = expiration_new(10);
    CHECK(e);

    uint64_t old_last_seen_date = expiration_last_time_seen(e);

    expiration_update_last_time_seen(e, uint64_t(zclock_time() / 1000));
    CHECK(expiration_last_time_seen(e) != old_last_seen_date);

    // from past!!
    old_last_seen_date = expiration_last_time_seen(e);
    expiration_update_last_time_seen(e, uint64_t(zclock_time() / 1000 - 10000));
    CHECK(expiration_last_time_seen(e) == old_last_seen_date);

    expiration_update_ttl(e, 1);
    CHECK(expiration_ttl(e) == 1);

    expiration_update_ttl(e, 10);
    CHECK(expiration_ttl(e) == 1); // because 10 > 1

    CHECK(expiration_time(e) == old_last_seen_date + 1 * 2);

    expiration_destroy(&e);
    CHECK(!e);
}

TEST_CASE("expiration test2")
{
    expiration_t* e = expiration_new(10);
    CHECK(e);

    expiration_update_ttl(e, 10);
    expiration_update_last_time_seen(e, 100);
    expiration_maintenance_set(e, 0);

    CHECK(expiration_ttl(e) == 10);
    CHECK(expiration_last_time_seen(e) == 100);
    CHECK(expiration_maintenance(e) == 0);

    CHECK(expiration_time(e) == 120); //time + 2*ttl

    expiration_maintenance_set(e, 100);
    CHECK(expiration_maintenance(e) == 100);
    CHECK(expiration_time(e) == 120); //time + 2*ttl

    expiration_maintenance_set(e, 1000);
    CHECK(expiration_maintenance(e) == 1000);
    CHECK(expiration_time(e) == 1000); // maintenance time

    expiration_update_last_time_seen(e, 2000);
    CHECK(expiration_maintenance(e) == 1000);
    CHECK(expiration_time(e) == 2020); //time + 2*ttl

    CHECK(expiration_maintenance(e) == 0); // auto reset

    expiration_destroy(&e);
    CHECK(!e);
}
