cmake_minimum_required(VERSION 3.7)

add_definitions(-DSINGLECOREAPPLICATION)

set(SOURCES ../singleapplication_t.cpp ../singleapplication_p.cpp)
set(HEADERS ../singleapplication_t.h ../singleapplication_p.h)
qt_wrap_cpp(MOC ${HEADERS})
add_library(singlecoreapplication STATIC ${SOURCES} ${MOC})
target_include_directories(singlecoreapplication PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}/..
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(singlecoreapplication PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Network
)
