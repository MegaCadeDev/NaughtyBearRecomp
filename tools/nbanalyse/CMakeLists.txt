cmake_minimum_required(VERSION 3.20)
project(nbanalyse CXX)

set(CMAKE_CXX_STANDARD 20)

# Reuse XenonRecomp's XEX loader and PowerPC disassembler.
add_subdirectory(../XenonRecomp XenonRecomp EXCLUDE_FROM_ALL)

add_executable(nb_switch_tables nb_switch_tables.cpp)
target_link_libraries(nb_switch_tables PRIVATE XenonUtils fmt::fmt)

add_executable(bctr_survey bctr_survey.cpp)
target_link_libraries(bctr_survey PRIVATE XenonUtils fmt::fmt)

add_executable(disas disas.cpp)
target_link_libraries(disas PRIVATE XenonUtils fmt::fmt)

add_executable(find_bytes find_bytes.cpp)
target_link_libraries(find_bytes PRIVATE XenonUtils fmt::fmt)

add_executable(imports imports.cpp)
target_link_libraries(imports PRIVATE XenonUtils fmt::fmt)
