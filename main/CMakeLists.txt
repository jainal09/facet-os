# Secrets come from the .env at the project root, never from a checked-in
# header. Parsed here at configure time and written to build/generated/
# credentials.h; CMAKE_CONFIGURE_DEPENDS makes an edit to .env re-run this, so
# a plain `idf.py build` always reflects the current values.

set(FACET_ENV_FILE "${CMAKE_SOURCE_DIR}/.env")
set(FACET_GEN_DIR  "${CMAKE_BINARY_DIR}/generated")

# Defaults, so a fresh clone with no .env still configures and builds. Empty
# credentials just mean no Wi-Fi join and no Unsplash fetch — the firmware
# already treats both as optional.
set(WIFI_SSID        "")
set(WIFI_PASSWORD    "")
set(STT_ENDPOINT_URL "https://example.com/")
set(STT_BEARER_TOKEN "")
set(TIMEZONE         "UTC0")
set(NTP_SERVER       "pool.ntp.org")
set(UNSPLASH_KEY     "")
set(UNSPLASH_QUERY   "space,nebula,astronomy")
set(BROKER_TOKEN         "")
set(BROKER_URL           "")
set(SPOTIFY_REFRESH_TOKEN  "")
set(SPOTIFY_CLIENT_ID    "")

# Seed a .env on first configure so the file always exists from here on.
# CMAKE_CONFIGURE_DEPENDS cannot watch a path that does not exist yet, so
# without this the fresh-clone flow — build once with no .env, then create one
# and build again — silently reuses the stale header and the device boots with
# no credentials. .env is gitignored, so writing it here cannot dirty the repo.
if(NOT EXISTS "${FACET_ENV_FILE}" AND EXISTS "${CMAKE_SOURCE_DIR}/.env.example")
    configure_file("${CMAKE_SOURCE_DIR}/.env.example" "${FACET_ENV_FILE}" COPYONLY)
    message(WARNING
        "Created ${FACET_ENV_FILE} from .env.example. "
        "Fill in WIFI_SSID and WIFI_PASSWORD, then build again — until you do, "
        "the device will boot without network credentials.")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FACET_ENV_FILE}")

if(EXISTS "${FACET_ENV_FILE}")
    file(STRINGS "${FACET_ENV_FILE}" _facet_env_lines)
    foreach(_line IN LISTS _facet_env_lines)
        if(_line MATCHES "^[ \t]*(#|$)")
            continue()
        endif()
        if(_line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
            set(_key "${CMAKE_MATCH_1}")
            set(_val "${CMAKE_MATCH_2}")
            string(STRIP "${_val}" _val)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" _val "${_val}")
            string(REGEX REPLACE "^'(.*)'$"   "\\1" _val "${_val}")
            # the value lands inside a C string literal
            string(REPLACE "\\" "\\\\" _val "${_val}")
            string(REPLACE "\"" "\\\"" _val "${_val}")
            set(${_key} "${_val}")
        endif()
    endforeach()
else()
    message(WARNING
        "No .env found at ${FACET_ENV_FILE}. "
        "Copy .env.example to .env and fill it in, or the device will boot "
        "with no Wi-Fi credentials.")
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/credentials.h.in"
               "${FACET_GEN_DIR}/credentials.h" @ONLY)

idf_component_register(
    SRCS "main.c" "ble_prov.c" "hud_clock_76.c" "hud_clock_48.c" "hud_text_18.c" "app_icons_64.c" "hud_icons_30.c"
    INCLUDE_DIRS "." "${FACET_GEN_DIR}"
    # UI sounds live in flash, not on the card: ~99 KB of a 2.4 MB free app
    # slot, and no dependency on the SD card or the network to make a noise.
    # Authored by assets/sounds/render.py — see that file for why.
    EMBED_FILES "../assets/sounds/tick.wav"
                "../assets/sounds/start.wav"
                "../assets/sounds/pause.wav"
                "../assets/sounds/resume.wav"
                "../assets/sounds/done.wav"
    REQUIRES bt nvs_flash esp_wifi esp_event esp_netif esp_http_client mbedtls esp_hw_support
             esp_timer esp_driver_gpio esp_driver_i2c esp_lcd_touch fatfs json esp_codec_dev esp_driver_i2s esp_app_format
             esp32_s3_touch_amoled_2_16 espressif__esp_lv_decoder
)
