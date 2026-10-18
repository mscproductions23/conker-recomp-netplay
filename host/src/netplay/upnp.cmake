# miniupnpc, for upnp.cpp: asking the router to forward the host's port. Static, nothing installed.
include(FetchContent)
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniupnp
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG miniupnpc_2_3_3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR miniupnpc)
FetchContent_MakeAvailable(miniupnp)
add_library(conker_upnp STATIC ${CMAKE_CURRENT_LIST_DIR}/upnp.cpp)
target_compile_features(conker_upnp PUBLIC cxx_std_20)
target_link_libraries(conker_upnp PUBLIC libminiupnpc-static)
add_executable(upnp_probe EXCLUDE_FROM_ALL ${CMAKE_CURRENT_LIST_DIR}/upnp_probe.cpp)
target_link_libraries(upnp_probe PRIVATE conker_upnp)
