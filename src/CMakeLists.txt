add_library(xplthread SHARED
  thread.c
)

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ALL_CFLAGS} ${PTHREAD_CFLAGS} ${OPENSSL_CFLAGS}")


target_link_libraries(xplthread
	xpllock${BIT}
)

install(TARGETS xplthread DESTINATION /usr/lib)
