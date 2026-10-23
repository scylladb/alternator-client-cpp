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

MAKEFILE_PATH := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CMAKE ?= cmake
CTEST ?= ctest
HAWKEYE ?= hawkeye

BUILD_DIR ?= build
CHECK_BUILD_DIR ?= build-check
CHECK_CXX_FLAGS ?= -Wall -Wextra -Werror
BUILD_CMAKE_FLAGS ?= -DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=OFF
INTEGRATION_CMAKE_FLAGS ?= -DALTERNATOR_CLIENT_CPP_ENABLE_AWS=ON -DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=ON

COMPOSE := docker compose -f $(MAKEFILE_PATH)/test/docker-compose.yml

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
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -E Integration

.PHONY: build-integration
build-integration:
	$(CMAKE) -S . -B $(BUILD_DIR) $(INTEGRATION_CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -R "alternator_client_cpp_aws_.*tests_present"

.PHONY: test-integration
test-integration: build-integration scylla-start
	ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 \
	ALTERNATOR_CLIENT_CPP_CA_FILE=$(MAKEFILE_PATH)/test/scylla/db.crt \
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -R Integration

.PHONY: .prepare-cert
.prepare-cert:
	@[ -f "$(MAKEFILE_PATH)/test/scylla/db.key" ] || (echo "Prepare certificate" && cd "$(MAKEFILE_PATH)/test/scylla" && openssl req -subj "/C=US/ST=Denial/L=Springfield/O=Dis/CN=www.example.com" -addext "subjectAltName=IP:172.41.0.2,IP:172.41.0.3,IP:172.41.0.4" -x509 -newkey rsa:4096 -keyout db.key -out db.crt -days 3650 -nodes && chmod 644 db.key)

.PHONY: scylla-start
scylla-start: .prepare-cert
	@sudo sysctl -w fs.aio-max-nr=10485760
	$(COMPOSE) up -d --wait

.PHONY: scylla-stop
scylla-stop:
	$(COMPOSE) down

.PHONY: scylla-kill
scylla-kill:
	$(COMPOSE) kill

.PHONY: scylla-rm
scylla-rm:
	$(COMPOSE) rm -f

.PHONY: clean
clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR) $(CHECK_BUILD_DIR)

.PHONY: release-check-user release-check-version release-tag _release-tag release-verify-tag test-release-signing

# Require the effective publisher account before importing credentials or publishing.
release-check-user:
	+@set -eu; \
	if [ "$${GITHUB_ACTIONS:-}" = true ]; then \
		user=$${GITHUB_TRIGGERING_ACTOR:-$${GITHUB_ACTOR:-}}; \
	else \
		if ! user=$$(gh api user --jq .login); then \
			echo "Release requires GitHub user scylladb-publisher. Run gh auth login or gh auth switch." >&2; \
			exit 1; \
		fi; \
	fi; \
	if [ "$$user" != scylladb-publisher ]; then \
		echo "Release requires GitHub user scylladb-publisher (current: $$user)." >&2; \
		exit 1; \
	fi

release-check-version:
	+@set -eu; \
	version=$${RELEASE_VERSION:-}; \
	invalid() { echo 'Expected RELEASE_VERSION=X.Y.Z, X.Y.Z-alpha.1, -beta.1, or -rc.1' >&2; exit 1; }; \
	case "$$version" in ''|*[!0-9A-Za-z.-]*) invalid;; esac; \
	number='(0|[1-9][0-9]*)'; \
	printf '%s\n' "$$version" | grep -Eq "^$$number\.$$number\.$$number(-(alpha|beta|rc)\.$$number)?$$" || invalid

release-tag: _release-tag

_release-tag: release-check-user release-check-version
	@python3 scripts/release/signing.py exec sh -ec '\
	: "$${RELEASE_VERSION:?Set RELEASE_VERSION to the version to release}"; \
	commit=$$(git rev-parse HEAD); \
	tag="v$$RELEASE_VERSION"; \
	if ! git show-ref --verify --quiet "refs/tags/$$tag"; then \
		git tag -s "$$tag" -m "Release $$tag" "$$commit"; \
	fi; \
	python3 scripts/release/signing.py verify-tag "$$tag" "$$commit"'

# Public verification is available to every consumer without signing credentials.
release-verify-tag:
	@python3 scripts/release/signing.py verify-tag "$${RELEASE_TAG:?Set RELEASE_TAG to the tag to verify}"

test-release-signing:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py' -v
