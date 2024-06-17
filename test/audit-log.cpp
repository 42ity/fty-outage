#include <catch2/catch.hpp>
#include "src/audit_log.h"

TEST_CASE("audit-logs")
{
    // initialize log for auditability
    AuditLog::init("outage-alert-test");

    REQUIRE(AuditLog::getInstance() != nullptr);

    // logs audit, see /etc/fty/ftylog.cfg (requires privileges)
    audit_log_info("outage-alert-test audit test %s", "INFO");
    audit_log_error("outage-alert-test audit test %s", "ERROR");

    AuditLog::deinit();
}
