include(GoogleTest)

foreach(_test dyn_spin_rwlock dyn_c_hash_map dyn_c_vector)
  add_executable(test_${_test} test_${_test}.C)
  target_link_libraries(test_${_test} PRIVATE common Threads::Threads GTest::gtest
                                              GTest::gtest_main)
  gtest_discover_tests(test_${_test} PROPERTIES TIMEOUT 60)
endforeach()
