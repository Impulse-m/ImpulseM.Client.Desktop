# ImpulseM unit tests. Qt-free: the core sources are compiled into impulse_tests only.
# Each test file under mtproto/impulse/tests/*.cpp is picked up by the glob.

add_executable(impulse_tests)
init_target(impulse_tests "(tests)")

target_include_directories(impulse_tests PRIVATE ${src_loc})

file(GLOB impulse_test_sources
    RELATIVE ${src_loc}
    CONFIGURE_DEPENDS
    ${src_loc}/mtproto/impulse/tests/*.cpp
)

nice_target_sources(impulse_tests ${src_loc}
PRIVATE
    mtproto/impulse/tests/impulse_test.h
    ${impulse_test_sources}
    ${impulse_core_sources}
)

target_link_libraries(impulse_tests
PRIVATE
    desktop-app::external_zlib
)
