cmake_minimum_required(VERSION 3.13)
project(hydrasdr_rfone_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # Lib path
    target_link_directories(hydrasdr_rfone_source PRIVATE "C:/Program Files/PothosSDR/bin/")

    target_include_directories(hydrasdr_rfone_source PUBLIC "C:/Program Files/PothosSDR/include/libhydrasdr/")

    target_link_libraries(hydrasdr_rfone_source PRIVATE hydrasdr)
elseif (ANDROID)
    target_include_directories(hydrasdr_rfone_source PUBLIC
        /sdr-kit/${ANDROID_ABI}/include/libhydrasdr
    )

    target_link_libraries(hydrasdr_rfone_source PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libusb1.0.so
        /sdr-kit/${ANDROID_ABI}/lib/libhydrasdr.so
    )
else (MSVC)
    find_package(PkgConfig)

    pkg_check_modules(LIBHYDRASDR_RFONE REQUIRED libhydrasdr)

    target_include_directories(hydrasdr_rfone_source PRIVATE ${LIBHYDRASDR_RFONE_INCLUDE_DIRS})
    target_link_directories(hydrasdr_rfone_source PRIVATE ${LIBHYDRASDR_RFONE_LIBRARY_DIRS})
    target_link_libraries(hydrasdr_rfone_source PRIVATE ${LIBHYDRASDR_RFONE_LIBRARIES})

    # Include it because for some reason pkgconfig doesn't look here?
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        target_include_directories(hydrasdr_rfone_source PRIVATE "/usr/local/include")
    endif()

endif ()