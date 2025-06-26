set(requires
    hardware_driver
)

idf_component_register(SRCS
  "app_main.c" "camera.c" "audio.c" "settings.h" "bsp_board.h" "bsp_board.c" "esp32_s3_korvo_2_v3_board.h"
  INCLUDE_DIRS "." REQUIRES mbedtls REQUIRES nvs_flash REQUIRES ${requires} REQUIRES esp-tls
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
