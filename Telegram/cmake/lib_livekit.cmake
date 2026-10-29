# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# LiveKit FFI for calls on Windows. The prepare stage livekit_ffi leaves
# livekit_ffi.dll and its C header under Libraries/win64/livekit. The DLL is
# copied next to the executable in modules/x64/livekit by a POST_BUILD step in
# Telegram/CMakeLists.txt, and it is loaded there by its full path only.
if (WIN32)
    set(livekit_ffi_dir ${libs_loc}/livekit)
    set(livekit_ffi_dll ${livekit_ffi_dir}/livekit_ffi.dll)
    set(livekit_ffi_modules_subdir modules/x64/livekit)
endif()
