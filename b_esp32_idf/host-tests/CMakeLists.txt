cmake_minimum_required(VERSION 3.20)
project(b_protocol_regression LANGUAGES C CXX)
include(CTest)
option(ENABLE_SANITIZERS "Address/undefined behavior checks on supported hosts" OFF)
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_executable(protocol_regression
  tests.cpp
  "${ROOT}/components/sensor_protocol/protocol.cpp"
  "${ROOT}/components/sensor_protocol/protocol_self_test.cpp"
  "${ROOT}/components/uart_link/session_gate.cpp"
  "${ROOT}/components/uart_link/session_wire.c")
target_include_directories(protocol_regression PRIVATE
  "${ROOT}/components/sensor_protocol/include"
  "${ROOT}/components/uart_link" "${ROOT}/components/uart_link/include")
target_compile_features(protocol_regression PRIVATE c_std_11 cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(protocol_regression PRIVATE -Wall -Wextra -Werror)
endif()
if(ENABLE_SANITIZERS AND NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(protocol_regression PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(protocol_regression PRIVATE -fsanitize=address,undefined)
endif()
set(CASES legacy crc_known_vector golden_wire_vectors every_split_boundary sticky_frames
  bad_crc_recovery noise_resynchronization ring_buffer_wrap
  session_existing_selftest session_request_retry session_confirmation_retry
  session_stm32_reboot session_b_reboot session_old_token session_sequence_wrap
  session_offer_expiry session_invalid_joystick session_legacy_gate
  codec_encoder_bounds codec_sensor_validation codec_parser_bounds
  random_fragmentation random_crc_recovery random_session_dedup random_cross_codec)
foreach(case IN LISTS CASES)
  add_test(NAME ${case} COMMAND protocol_regression ${case})
  set_tests_properties(${case} PROPERTIES TIMEOUT 20)
endforeach()
