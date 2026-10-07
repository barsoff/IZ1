cmake_minimum_required(VERSION 3.25)
project(IZ1 C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(IZ1 main.c battle.c config.c output.c)
set_target_properties(IZ1 PROPERTIES OUTPUT_NAME tanks)
target_compile_definitions(IZ1 PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(IZ1 PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME battle_checks COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:IZ1>)
set_tests_properties(battle_checks PROPERTIES TIMEOUT 30)
