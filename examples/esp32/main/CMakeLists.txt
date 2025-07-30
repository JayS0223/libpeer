idf_component_register(SRCS
  "app_main.c" "camera.c"
  INCLUDE_DIRS "."
  REQUIRES mbedtls  REQUIRES srtp REQUIRES json  REQUIRES esp_netif  REQUIRES fatfs  REQUIRES vfs REQUIRES  esp_common REQUIRES esp_timer  REQUIRES esp_lcd  REQUIRES nvs_flash
)

set(EXTRA_COMPONENT_DIRS
    ../../components
    )
list(APPEND EXTRA_COMPONENT_DIRS "../../components")  
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
