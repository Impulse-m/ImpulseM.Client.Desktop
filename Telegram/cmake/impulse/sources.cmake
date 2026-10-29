# ImpulseM source lists. Directory globs with CONFIGURE_DEPENDS: each task adds
# files to these directories without editing Telegram/CMakeLists.txt.
# Paths are relative to src_loc (Telegram/SourceFiles), as nice_target_sources expects.

file(GLOB impulse_core_sources
    RELATIVE ${src_loc}
    CONFIGURE_DEPENDS
    ${src_loc}/mtproto/impulse/core/*.cpp
)

file(GLOB impulse_glue_sources
    RELATIVE ${src_loc}
    CONFIGURE_DEPENDS
    ${src_loc}/mtproto/impulse/glue/*.cpp
)

file(GLOB impulse_calls_sources
    RELATIVE ${src_loc}
    CONFIGURE_DEPENDS
    ${src_loc}/calls/livekit/*.cpp
    ${src_loc}/calls/group/livekit_*.cpp
)

file(GLOB impulse_proxy_sources
    RELATIVE ${src_loc}
    CONFIGURE_DEPENDS
    ${src_loc}/core/impulse_proxy/*.cpp
)

nice_target_sources(Telegram ${src_loc}
PRIVATE
    ${impulse_core_sources}
    ${impulse_glue_sources}
    ${impulse_calls_sources}
    ${impulse_proxy_sources}
)
