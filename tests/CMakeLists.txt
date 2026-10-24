find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME end_to_end COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py $<TARGET_FILE:pdw-cli>)
endif()
