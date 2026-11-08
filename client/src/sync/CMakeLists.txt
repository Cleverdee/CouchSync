# Qt-free sync core: clock sync, room state, drift controller.
add_library(couchsync_sync STATIC
  ClockSync.h ClockSync.cpp
  PingSchedule.h
  RoomState.h
  SyncEngine.h SyncEngine.cpp
)
target_include_directories(couchsync_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(couchsync_sync PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(couchsync_sync PRIVATE /utf-8 /W4)
else()
  target_compile_options(couchsync_sync PRIVATE -Wall -Wextra -Wpedantic)
endif()
