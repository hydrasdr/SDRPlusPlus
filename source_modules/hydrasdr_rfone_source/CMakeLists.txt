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
        # macOS bundle fix for issue HydraSDR RFOne did not display the device in the source list
        if(USE_BUNDLE_DEFAULTS)
            # Set rpath for bundle structure
            set_target_properties(hydrasdr_rfone_source PROPERTIES
                INSTALL_RPATH "@executable_path/../Frameworks"
                BUILD_WITH_INSTALL_RPATH TRUE
            )
            # Copy libhydrasdr to build frameworks for bundle packaging
            add_custom_command(TARGET hydrasdr_rfone_source POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Frameworks
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${LIBHYDRASDR_RFONE_LIBRARY_DIRS}/libhydrasdr.dylib
                    ${CMAKE_BINARY_DIR}/Frameworks/
                COMMAND install_name_tool -change @rpath/libhydrasdr.dylib
                    @executable_path/../Frameworks/libhydrasdr.dylib
                    $<TARGET_FILE:hydrasdr_rfone_source>
                COMMENT "Preparing HydraSDR for macOS bundle"
            )
        endif()
    endif()

endif ()
