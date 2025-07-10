
idf_component_register(SRCS
  "app_main.c" "camera.c" "settings.h" "board.c"
  INCLUDE_DIRS "." REQUIRES mbedtls REQUIRES nvs_flash REQUIRES ${requires} REQUIRES esp-tls 
)
spiffs_create_partition_image(my_spiffs spiffs_image FLASH_IN_PROJECT "spiffs")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
