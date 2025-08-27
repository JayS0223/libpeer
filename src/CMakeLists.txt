project(lib)

file(GLOB SRCS "dtls_srtp.c" "ports.c" "peer_connection.c" "peer_signaling.c" "board.c" "videosdk.c" "audio.c" "rtcp.c" "*.c")

file(GLOB HEADERS "peer.h" "peer_connection.h" "agent.c" "peer_signaling.h")

add_library(peer
  ${SRCS}
  ${HTTP_SOURCES}
  ${MQTT_SOURCES}
  ${MQTT_SERIALIZER_SOURCES}
)

include_directories(peer PUBLIC
  ${HTTP_INCLUDE_PUBLIC_DIRS}
  ${MQTT_INCLUDE_PUBLIC_DIRS}
)

add_dependencies(peer cjson mbedtls usrsctp srtp2)

target_link_libraries(peer ${DEP_LIBS})

set_target_properties(peer PROPERTIES PUBLIC_HEADER "${HEADERS}")

install(TARGETS peer
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_PREFIX}/include/
)

ude/
)

