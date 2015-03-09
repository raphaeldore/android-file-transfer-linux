project(mtp)
cmake_minimum_required(VERSION 2.8)
set(CMAKE_USE_RELATIVE_PATHS TRUE)

include(FindPkgConfig)
find_package( Threads )

pkg_check_modules(LIBUSB libusb REQUIRED)

add_definitions(-Wall -fstrict-aliasing -Wno-variadic-macros -pthread -std=c++1y)
include_directories(. ${CMAKE_SOURCE_DIR} ${LIBUSB_INCLUDE_DIRS})

set(SOURCES
	mtp/usb/Context.cpp
	mtp/usb/Device.cpp
	mtp/usb/DeviceDescriptor.cpp
	mtp/usb/Exception.cpp

	mtp/main.cpp
)

add_executable(mtp-test ${SOURCES})
target_link_libraries(mtp-test -lusb-1.0 ${CMAKE_THREAD_LIBS_INIT})