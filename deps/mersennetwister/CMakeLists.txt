add_library(mersennetwister INTERFACE)

# Group sources
GroupSources(${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(mersennetwister
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
