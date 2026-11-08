# Headless sync simulator (docs/DESIGN.md, "Testing"): fake clients with network impairment against the
# real room server. Console app, no GUI. See client/README.md.
qt_add_executable(couchsync_sim
  main.cpp
  Scenario.h Scenario.cpp
  Proxy.h Proxy.cpp
  SimClient.h SimClient.cpp
  Common.h
  # WsConn is reused as-is (it only needs Qt Core/Network).
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net/WsConn.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net/WsConn.cpp
)
target_include_directories(couchsync_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net)
target_link_libraries(couchsync_sim PRIVATE Qt6::Core Qt6::Network couchsync_sync)

if(MSVC)
  target_compile_options(couchsync_sim PRIVATE /utf-8 /W4)
endif()

if(WIN32)
  option(COUCHSYNC_DEPLOY_SIM "Run windeployqt next to couchsync_sim.exe after build" ON)
  if(COUCHSYNC_DEPLOY_SIM)
    add_custom_command(TARGET couchsync_sim POST_BUILD
      COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime $<TARGET_FILE:couchsync_sim>
      COMMENT "Deploying Qt runtime for couchsync_sim"
      VERBATIM)
  endif()
endif()
