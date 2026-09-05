rawspeed_add_executable(rawspeed-cli main.cpp)

target_link_libraries(rawspeed-cli PRIVATE rawspeed)

if(MSVC)
  set(_rawspeed_msvc_compat_header
      "${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat.h")
  target_compile_definitions(rawspeed-cli PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(rawspeed-cli PRIVATE
                         "/FI${_rawspeed_msvc_compat_header}")

  get_target_property(_rawspeed_dependencies rawspeed LINK_LIBRARIES)
  foreach(_rawspeed_dependency IN LISTS _rawspeed_dependencies)
    if(TARGET "${_rawspeed_dependency}")
      get_target_property(_rawspeed_dependency_type
                          "${_rawspeed_dependency}" TYPE)
      if(_rawspeed_dependency_type STREQUAL "OBJECT_LIBRARY")
        target_compile_options(
          "${_rawspeed_dependency}" PRIVATE
          "/FI${_rawspeed_msvc_compat_header}")
      endif()
    endif()
  endforeach()
endif()

install(TARGETS rawspeed-cli DESTINATION ${CMAKE_INSTALL_BINDIR})
