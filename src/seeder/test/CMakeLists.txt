# Copyright (c) 2019 The Bitcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

project(bitcoin-seeder-test)

include(TestSuite)
create_test_suite(bitcoin-seeder)
add_dependencies(check check-bitcoin-seeder)

add_boost_unit_tests_to_suite(bitcoin-seeder test_bitcoin-seeder
  seeder_tests.cpp
)

target_link_libraries(test_bitcoin-seeder
  seeder-netprocessing
)
