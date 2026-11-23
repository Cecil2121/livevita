cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK environment variable is not set.")
  endif()
endif()

project(LiveVita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "LiveVita")
set(VITA_TITLEID  "LVTW00001")
set(VITA_VERSION  "02.00")
set(APP_VERSION   "v2.0.0")

# Your Twitch application's Client ID (dev.twitch.tv/console, Client Type: Public).
# Optional here: without it, the app reads ux0:data/LiveVita/client_id.txt.
set(LV_CLIENT_ID "" CACHE STRING "Twitch Client ID built into the app")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
# room for vita-elf-create's data (newer VitaSDK linkers; older ones ignore the symbol)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=__sce_headroom=0x4000")

# The video player engine comes from Vita IPTV (MIT License, github.com/emrhky/VitaIPTV).
set(PLAYER_SRC
  src/player/tsplayer.c
  src/player/tsdemux.c
  src/player/mkvdemux.c
  src/player/flvdemux.c
  src/player/mpadec.c
  src/player/a52dec.c
  src/player/vsurr.c
  src/player/hls.c
  src/player/spsfix.c
  src/player/curlio.c
  src/player/vod.c
  src/player/srt.c
  src/player/httpio.c
  src/player/audiofx.c
  src/player/aac51.c
)

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/home.c
  src/hh.c
  src/tv.c
  src/favcat.c
  src/favchan.c
  src/tpemote.c
  src/jingle.c
  src/playerui.c
  src/ui.c
  src/chat.c
  src/imgcache.c
  src/twitch.c
  src/json.c
  src/applog.c
  ${PLAYER_SRC}
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
  "APP_VERSION=\"${APP_VERSION}\""
  "LV_CLIENT_ID=\"${LV_CLIENT_ID}\"")

# HTTPS with the built-in Mbed TLS (the Vita's own TLS is too old for Twitch)
file(GLOB MBEDTLS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/library/*.c)
target_sources(${PROJECT_NAME}.elf PRIVATE ${MBEDTLS_SRC})
target_include_directories(${PROJECT_NAME}.elf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsmn
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE HAVE_TLS "MBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"")

file(GLOB EXTRA_STUBS
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceSharedFb*_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceAppMgr_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceIme_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceKernelModulemgr_stub.a")

target_link_libraries(${PROJECT_NAME}.elf
  vita2d
  freetype png jpeg z m pthread
  SceRtc_stub
  SceAvPlayer_stub SceAudio_stub SceVideodec_stub SceCodecEngine_stub SceAudiodec_stub
  SceNet_stub SceNetCtl_stub SceHttp_stub SceSsl_stub
  SceCtrl_stub SceTouch_stub SceSysmodule_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub
  ScePgf_stub ScePvf_stub SceAppUtil_stub ScePower_stub SceAVConfig_stub SceRegistryMgr_stub
  ${EXTRA_STUBS}
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE jingle.raw jingle.raw
  FILE LICENSE LICENSE.txt
  FILE THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.txt
  FILE third_party/VitaIPTV-LICENSE.txt licenses/VitaIPTV-LICENSE.txt
  FILE third_party/mbedtls/LICENSE licenses/mbedtls-LICENSE.txt
  FILE third_party/jsmn/LICENSE licenses/jsmn-LICENSE.txt
)
