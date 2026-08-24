# ESP32-S3 PIE (SIMD) blenders for LVGL's software renderer, vendored from
# espressif/esp_lvgl_port 2.9.0 (Apache-2.0, LICENSE alongside). Only the
# assembly and its wrapper header are taken; none of esp_lvgl_port's runtime.
#
# LVGL calls these through LV_DRAW_SW_ASM_CUSTOM_INCLUDE only on the plain
# fill / opaque-copy path (mask == NULL && opa >= LV_OPA_MAX); every other
# case still runs LVGL's C. Upstream gates this on LVGL 9.1-9.2, but the
# wrappers copy named fields into their own asm_dsc_t, so the assembly never
# sees LVGL's struct layout; the one 9.5 difference is the typedef spelling,
# patched in the header.
idf_component_register(
    SRCS "lv_color_blend_to_rgb565_esp32s3.S"
         "lv_color_blend_to_argb8888_esp32s3.S"
         "lv_color_blend_to_rgb888_esp32s3.S"
         "lv_rgb565_blend_normal_to_rgb565_esp32s3.S"
         "lv_rgb888_blend_normal_to_rgb888_esp32s3.S"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl)

# LVGL's blend .c files #include LV_DRAW_SW_ASM_CUSTOM_INCLUDE, so the header
# has to be visible from inside the lvgl component, not just from ours.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")

# Nothing in C references these symbols by name until the macros expand
# inside lvgl, which is linked before us; force them in.
foreach(sym lv_color_blend_to_rgb565_esp lv_color_blend_to_argb8888_esp
            lv_color_blend_to_rgb888_esp lv_rgb565_blend_normal_to_rgb565_esp
            lv_rgb888_blend_normal_to_rgb888_esp)
    set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-u ${sym}")
endforeach()
