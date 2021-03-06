#!/usr/bin/env bash

export LC_ALL=C.UTF-8

set -euxo pipefail

: "${ABC_BUILD_NAME:=""}"
if [ -z "$ABC_BUILD_NAME" ]; then
  echo "Error: Environment variable ABC_BUILD_NAME must be set"
  exit 1
fi

echo "Running build configuration '${ABC_BUILD_NAME}'..."

TOPLEVEL=$(git rev-parse --show-toplevel)
export TOPLEVEL

setup() {
  : "${BUILD_DIR:=${TOPLEVEL}/build}"
  mkdir -p "${BUILD_DIR}/output"
  BUILD_DIR=$(cd "${BUILD_DIR}"; pwd)
  export BUILD_DIR

  TEST_RUNNER_FLAGS="--tmpdirprefix=output"

  cd "${BUILD_DIR}"

  # Determine the number of build threads
  THREADS=$(nproc || sysctl -n hw.ncpu)
  export THREADS

  # Base directories for sanitizer related files 
  SAN_SUPP_DIR="${TOPLEVEL}/test/sanitizer_suppressions"
  SAN_LOG_DIR="/tmp/sanitizer_logs"

  # Create the log directory if it doesn't exist and clear it
  mkdir -p "${SAN_LOG_DIR}"
  rm -rf "${SAN_LOG_DIR:?}"/*

  # Sanitizers options, not used if sanitizers are not enabled
  export ASAN_OPTIONS="malloc_context_size=0:log_path=${SAN_LOG_DIR}/asan.log"
  export LSAN_OPTIONS="suppressions=${SAN_SUPP_DIR}/lsan:log_path=${SAN_LOG_DIR}/lsan.log"
  export TSAN_OPTIONS="suppressions=${SAN_SUPP_DIR}/tsan:log_path=${SAN_LOG_DIR}/tsan.log"
  export UBSAN_OPTIONS="suppressions=${SAN_SUPP_DIR}/ubsan:print_stacktrace=1:halt_on_error=1:log_path=${SAN_LOG_DIR}/ubsan.log"
}

# Facility to print out sanitizer log outputs to the build log console
print_sanitizers_log() {
  for log in "${SAN_LOG_DIR}"/*.log.*
  do
    echo "*** Output of ${log} ***"
    cat "${log}"
  done
}
trap "print_sanitizers_log" ERR

CI_SCRIPTS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
setup

case "$ABC_BUILD_NAME" in
  build-asan)
    # Build with the address sanitizer, then run unit tests and functional tests.
    CMAKE_FLAGS=(
      "-DCMAKE_BUILD_TYPE=Debug"
      "-DENABLE_SANITIZERS=address"
      "-DCCACHE=OFF"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check
    # FIXME Remove when wallet_multiwallet works with asan after backporting at least the following PRs from Core and their dependencies: 13161, 12493, 14320, 14552, 14760, 11911.
    TEST_RUNNER_FLAGS="${TEST_RUNNER_FLAGS} --exclude=wallet_multiwallet"
    ./test/functional/test_runner.py ${TEST_RUNNER_FLAGS}
    ;;

  build-ubsan)
    # Build with the undefined sanitizer, then run unit tests and functional tests.
    CMAKE_FLAGS=(
      "-DCMAKE_BUILD_TYPE=Debug"
      "-DENABLE_SANITIZERS=undefined"
      "-DCCACHE=OFF"
      "-DCMAKE_C_COMPILER=clang"
      "-DCMAKE_CXX_COMPILER=clang++"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check
    # FIXME Remove when abc-p2p-compactblocks works with ubsan.
    TEST_RUNNER_FLAGS="${TEST_RUNNER_FLAGS} --exclude=abc-p2p-compactblocks"
    ./test/functional/test_runner.py ${TEST_RUNNER_FLAGS}
    ;;

  build-tsan)
    # Build with the thread sanitizer, then run unit tests and functional tests.
    CMAKE_FLAGS=(
      "-DCMAKE_BUILD_TYPE=Debug"
      "-DENABLE_SANITIZERS=thread"
      "-DCCACHE=OFF"
      "-DCMAKE_C_COMPILER=clang"
      "-DCMAKE_CXX_COMPILER=clang++"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check
    # FIXME Remove when wallet_multiwallet works with tsan after backporting at least the following PRs from Core and their dependencies: 13161, 12493, 14320, 14552, 14760, 11911.
    TEST_RUNNER_FLAGS="${TEST_RUNNER_FLAGS} --exclude=wallet_multiwallet"
    ./test/functional/test_runner.py ${TEST_RUNNER_FLAGS}
    ;;

  build-default)
    # Build, run unit tests and functional tests (all extended tests if this is the master branch).
    "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check

    BRANCH=$(git rev-parse --abbrev-ref HEAD)
    if [[ "${BRANCH}" == "master" ]]; then
      TEST_RUNNER_FLAGS="${TEST_RUNNER_FLAGS} --extended"
    fi
    ./test/functional/test_runner.py ${TEST_RUNNER_FLAGS}
    ./test/functional/test_runner.py -J=junit_results_next_upgrade.xml --with-gravitonactivation ${TEST_RUNNER_FLAGS}

    # Build secp256k1 and run the java tests.
    export TOPLEVEL="${TOPLEVEL}"/src/secp256k1
    export BUILD_DIR="${TOPLEVEL}"/build
    setup
    CMAKE_FLAGS=(
      "-DSECP256K1_ENABLE_MODULE_ECDH=ON"
      "-DSECP256K1_ENABLE_JNI=ON"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check-secp256k1-java
    ;;

  build-without-wallet)
    # Build without wallet and run the unit tests.
    CMAKE_FLAGS=(
      "-DBUILD_BITCOIN_WALLET=OFF"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check
    ;;

  build-ibd)
    "${CI_SCRIPTS_DIR}"/build_cmake.sh
    "${CI_SCRIPTS_DIR}"/ibd.sh -disablewallet -debug=net
    ;;

  build-ibd-no-assumevalid-checkpoint)
    "${CI_SCRIPTS_DIR}"/build_cmake.sh
    "${CI_SCRIPTS_DIR}"/ibd.sh -disablewallet -assumevalid=0 -checkpoints=0 -debug=net
    ;;

  build-werror)
    # Build with variable-length-array and thread-safety-analysis treated as errors
    CMAKE_FLAGS=(
      "-DENABLE_WERROR=ON"
      "-DCMAKE_C_COMPILER=clang"
      "-DCMAKE_CXX_COMPILER=clang++"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ;;

  build-check-all)
    CMAKE_FLAGS=(
      "-DSECP256K1_ENABLE_MODULE_ECDH=ON"
      "-DSECP256K1_ENABLE_JNI=ON"
    )
    CMAKE_FLAGS="${CMAKE_FLAGS[*]}" "${CI_SCRIPTS_DIR}"/build_cmake.sh
    ninja check-all
    ;;

  build-autotools)
    # Ensure that the build using autotools is not broken
    "${CI_SCRIPTS_DIR}"/build_autotools.sh
    make -j "${THREADS}" check
    ;;

  check-seeds-mainnet)
    "${CI_SCRIPTS_DIR}"/build_cmake.sh
    "${CI_SCRIPTS_DIR}"/check-seeds.sh main 80
    ;;

  check-seeds-testnet)
    "${CI_SCRIPTS_DIR}"/build_cmake.sh
    "${CI_SCRIPTS_DIR}"/check-seeds.sh test 70
    ;;

  *)
    echo "Error: Invalid build name '${ABC_BUILD_NAME}'"
    exit 2
    ;;
esac
