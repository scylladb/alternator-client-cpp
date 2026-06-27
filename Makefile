MAKEFILE_PATH := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CMAKE ?= cmake
CTEST ?= ctest

BUILD_DIR ?= build
CHECK_BUILD_DIR ?= build-check
CHECK_CXX_FLAGS ?= -Wall -Wextra -Werror

COMPOSE := docker compose -f $(MAKEFILE_PATH)/test/docker-compose.yml

.PHONY: build
build:
	$(CMAKE) -S . -B $(BUILD_DIR)
	$(CMAKE) --build $(BUILD_DIR) --parallel

.PHONY: check
check:
	$(CMAKE) -S . -B $(CHECK_BUILD_DIR) -DCMAKE_CXX_FLAGS="$(CHECK_CXX_FLAGS)" -DALTERNATOR_CLIENT_CPP_ENABLE_AWS=OFF
	$(CMAKE) --build $(CHECK_BUILD_DIR) --parallel

.PHONY: test
test: build check test-unit test-integration

.PHONY: test-unit
test-unit:
	$(CMAKE) -S . -B $(BUILD_DIR)
	$(CMAKE) --build $(BUILD_DIR) --parallel
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -E Integration

.PHONY: test-integration
test-integration: scylla-start
	$(CMAKE) -S . -B $(BUILD_DIR)
	$(CMAKE) --build $(BUILD_DIR) --parallel
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
