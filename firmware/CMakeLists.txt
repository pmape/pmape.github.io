cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(padmapper)

target_sources(app PRIVATE src/main.c src/pad.c src/out.c src/engine.c src/cfg.c src/ble_out.c src/ble_cfg.c src/pm_hmac.c src/hid_in.c src/hid_map.c)
if(CONFIG_USB_MASS_STORAGE)
  target_sources(app PRIVATE src/vdisk.c)
endif()

# Kartın USB belleğine gömülen ayar sayfası (derleme sırasında assets/ içine konur)
if(CONFIG_USB_MASS_STORAGE)
  set(ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets)
  if(NOT EXISTS ${ASSETS}/padmapper.html)
    file(WRITE ${ASSETS}/padmapper.html "")
  endif()
  set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app ${ASSETS}/padmapper.html ${gen_dir}/html.inc)
endif()
