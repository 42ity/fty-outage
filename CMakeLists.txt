cmake_minimum_required(VERSION 3.13)
cmake_policy(VERSION 3.13)

project(fty-outage
  VERSION 1.0.0
  DESCRIPTION "Agent that sends alerts when device does not communicate"
  LANGUAGES CXX)

##############################################################################################################
find_package(fty-cmake PATHS ${CMAKE_BINARY_DIR}/fty-cmake)
##############################################################################################################

etn_target(static ${PROJECT_NAME}-lib
    SOURCES
        expiration.h
        expiration.cc
        data.h
        data.cc
        outage-metric.h
        outage-metric.cc
        fty-outage-server.h
        fty-outage-server.cc
    USES
        czmq
        mlm
        fty_proto
        fty_common
        fty_common_mlm
        fty_common_logging
        fty_shm
    PRIVATE
)

##############################################################################################################

etn_target(exe ${PROJECT_NAME}
    SOURCES
        fty-outage.cc
    USES
        ${PROJECT_NAME}-lib
)

##############################################################################################################

# Agent config
etn_configure_file(
    resources/${PROJECT_NAME}.cfg.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}"
)

# systemd unit
etn_configure_file(
    resources/${PROJECT_NAME}.service.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system/"
)

# systemd tmpfiles
etn_configure_file(
    resources/${PROJECT_NAME}.conf.in
    TARGET      ${PROJECT_NAME}
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d/"
)

##############################################################################################################

etn_test_target(${PROJECT_NAME}-lib
    SOURCES
        test/main.cpp
        test/expiration.cpp
        test/data.cpp
        test/outage-server-metric.cpp
        test/outage-server-alarm.cpp
    PREPROCESSOR
        -DCATCH_CONFIG_FAST_COMPILE
    SUBDIR
        test
)
