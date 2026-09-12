# Copyright ScyllaDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

SHELL := bash
.ONESHELL:
.SHELLFLAGS := -eo pipefail -c

MAKEFILE_PATH := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CMAKE ?= cmake
CTEST ?= ctest
HAWKEYE ?= hawkeye

BUILD_DIR ?= build
CHECK_BUILD_DIR ?= build-check
CHECK_CXX_FLAGS ?= -Wall -Wextra -Werror
BUILD_CMAKE_FLAGS ?= -DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=OFF
INTEGRATION_CMAKE_FLAGS ?= -DALTERNATOR_CLIENT_CPP_ENABLE_AWS=ON -DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=ON

SCYLLA_CCM_COMMIT := d15a2fab9d22fffad8a30c806a7c8e1632e58aae
SCYLLA_CCM_VENV := $(MAKEFILE_PATH)/.deps/scylla-ccm-$(SCYLLA_CCM_COMMIT)
PINNED_SCYLLA_CCM_PATH := $(SCYLLA_CCM_VENV)/bin/ccm
SCYLLA_CCM_INSTALL_LOCK := $(SCYLLA_CCM_VENV).install.lock
SCYLLA_CCM_INSTALL_MARKER := $(SCYLLA_CCM_VENV)/.install-complete
SCYLLA_CCM_PATH ?= $(PINNED_SCYLLA_CCM_PATH)
SCYLLA_CCM_EXECUTABLE := $(if $(findstring /,$(SCYLLA_CCM_PATH)),$(abspath $(SCYLLA_CCM_PATH)),$(SCYLLA_CCM_PATH))
SCYLLA_VERSION ?= release:2025.2.5
SCYLLA_INTEGRATION_VERSION ?= release:2026.1.6
SCYLLA_CCM_DIAGNOSTICS_DIR ?= $(abspath $(BUILD_DIR))/ccm
SCYLLA_CCM_NO_PROXY := localhost,127.0.0.1

.PHONY: build
build:
	$(CMAKE) -S . -B $(BUILD_DIR) $(BUILD_CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel

.PHONY: check
check: check-license-headers
	$(CMAKE) -S . -B $(CHECK_BUILD_DIR) -DCMAKE_CXX_FLAGS="$(CHECK_CXX_FLAGS)" -DALTERNATOR_CLIENT_CPP_ENABLE_AWS=OFF
	$(CMAKE) --build $(CHECK_BUILD_DIR) --parallel

.PHONY: check-license-headers
check-license-headers:
	$(HAWKEYE) check

.PHONY: test
test: build check test-unit test-integration

.PHONY: test-unit
test-unit:
	$(CMAKE) -S . -B $(BUILD_DIR) $(BUILD_CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -LE '^(Integration|CcmProvisioning)$$'

.PHONY: build-integration
build-integration:
	$(CMAKE) -S . -B $(BUILD_DIR) $(INTEGRATION_CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -R "alternator_client_cpp_aws_.*tests_present"

.PHONY: test-integration
test-integration: build-integration ccm-install
	@for command in openssl setsid; do
		executable=$$(type -P -- "$$command" || true)
		[[ -n "$$executable" && -x "$$executable" ]] || {
			echo "An external $$command executable is required for CCM integration tests" >&2
			exit 1
		}
	done
	ccm_executable="$(SCYLLA_CCM_EXECUTABLE)"
	[[ "$$ccm_executable" == */* ]] \
		|| ccm_executable=$$(type -P -- "$$ccm_executable")
	[[ "$$ccm_executable" == /* ]] \
		|| ccm_executable="$$(pwd -P)/$$ccm_executable"
	ccm_no_proxy="$(SCYLLA_CCM_NO_PROXY)"
	for ccm_id in {1..99}; do
		for node_id in {1..9}; do
			ccm_no_proxy+=",127.0.$$ccm_id.$$node_id"
		done
	done
	[[ -z "$${NO_PROXY:-}" ]] || ccm_no_proxy+=",$$NO_PROXY"
	[[ -z "$${no_proxy:-}" ]] || ccm_no_proxy+=",$$no_proxy"
	NO_PROXY="$$ccm_no_proxy" \
	no_proxy="$$ccm_no_proxy" \
	ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 \
	SCYLLA_VERSION="$(SCYLLA_VERSION)" \
	SCYLLA_CCM_PATH="$$ccm_executable" \
	SCYLLA_CCM_DIAGNOSTICS_DIR="$(SCYLLA_CCM_DIAGNOSTICS_DIR)" \
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --no-tests=error -L '^CcmProvisioning$$'
	NO_PROXY="$$ccm_no_proxy" \
	no_proxy="$$ccm_no_proxy" \
	ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 \
	SCYLLA_VERSION="$(SCYLLA_INTEGRATION_VERSION)" \
	SCYLLA_CCM_PATH="$$ccm_executable" \
	SCYLLA_CCM_DIAGNOSTICS_DIR="$(SCYLLA_CCM_DIAGNOSTICS_DIR)" \
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --no-tests=error -L '^Integration$$'

.PHONY: ccm-install
ccm-install:
	@ccm_works() {
		local executable=$$1
		[[ "$$executable" == */* ]] || executable=$$(type -P -- "$$executable" || true)
		[[ -n "$$executable" && -f "$$executable" && -x "$$executable" ]] \
			&& "$$executable" create --help >/dev/null 2>&1
	}
	install_complete() {
		[[ -f "$(SCYLLA_CCM_INSTALL_MARKER)" \
			&& ! -L "$(SCYLLA_CCM_INSTALL_MARKER)" ]] || return 1
		[[ "$$(< "$(SCYLLA_CCM_INSTALL_MARKER)")" == "$(SCYLLA_CCM_COMMIT)" ]] || return 1
		ccm_works "$(PINNED_SCYLLA_CCM_PATH)"
	}
	if [[ "$(SCYLLA_CCM_EXECUTABLE)" != "$(PINNED_SCYLLA_CCM_PATH)" ]]; then
		ccm_works "$(SCYLLA_CCM_EXECUTABLE)" || {
			echo "SCYLLA_CCM_PATH is not a working CCM executable: $(SCYLLA_CCM_EXECUTABLE)" >&2
			exit 1
		}
		echo "Using CCM executable: $(SCYLLA_CCM_EXECUTABLE)"
		exit 0
	fi
	command -v flock >/dev/null 2>&1 || {
		echo "flock is required to install scylla-ccm" >&2
		exit 1
	}
	mkdir -p -- "$(dir $(SCYLLA_CCM_VENV))"
	exec {install_lock_fd}>"$(SCYLLA_CCM_INSTALL_LOCK)"
	flock -x "$$install_lock_fd"
	if install_complete; then
		echo "Using CCM executable: $(PINNED_SCYLLA_CCM_PATH)"
		exit 0
	fi
	command -v uv >/dev/null 2>&1 || {
		echo "uv is required to install scylla-ccm: https://docs.astral.sh/uv/" >&2
		exit 1
	}
	uv venv --clear "$(SCYLLA_CCM_VENV)"
	uv pip install --python "$(SCYLLA_CCM_VENV)/bin/python" \
		"git+https://github.com/scylladb/scylla-ccm.git@$(SCYLLA_CCM_COMMIT)"
	ccm_works "$(PINNED_SCYLLA_CCM_PATH)" || {
		echo "Installed CCM entry point failed its launch check" >&2
		exit 1
	}
	temporary_marker=$$(mktemp "$(SCYLLA_CCM_VENV)/.install-complete.XXXXXXXX")
	printf '%s\n' "$(SCYLLA_CCM_COMMIT)" > "$$temporary_marker"
	chmod 600 -- "$$temporary_marker"
	mv -fT -- "$$temporary_marker" "$(SCYLLA_CCM_INSTALL_MARKER)"
	echo "Using CCM executable: $(PINNED_SCYLLA_CCM_PATH)"

.PHONY: clean
clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR) $(CHECK_BUILD_DIR)
