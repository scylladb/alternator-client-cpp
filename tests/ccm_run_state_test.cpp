/*
 * Copyright ScyllaDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "testinfra/test_cluster_internal.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace scylladb::alternator::testinfra;

namespace {

namespace fs = std::filesystem;

class ScopedFd final {
public:
    ScopedFd() = default;

    explicit ScopedFd(int fd)
        : fd_(fd) {}

    ~ScopedFd() {
        Reset();
    }

    ScopedFd(ScopedFd&& other) noexcept
        : fd_(other.Release()) {}

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    [[nodiscard]] int Get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int Release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

    void Reset(int replacement = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = replacement;
    }

private:
    int fd_ = -1;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/alternator-client-cpp-run-state-test.XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path_ = fs::path(created).lexically_normal();
    }

    ~TemporaryDirectory() {
        const auto name = path_.filename().string();
        if (path_.parent_path() != fs::path("/tmp") ||
            name.compare(0, std::strlen("alternator-client-cpp-run-state-test."),
                         "alternator-client-cpp-run-state-test.") != 0) {
            return;
        }
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const fs::path& Path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::system_error(errno, std::generic_category(), "setenv");
        }
    }

    ~ScopedEnvironment() {
        if (previous_) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedMetadataPublishFsyncFailure final {
public:
    explicit ScopedMetadataPublishFsyncFailure(
        std::uint32_t countdown,
        bool fail_rollback_fsync = false) {
        SetCcmRunStateMetadataPublishFsyncFailureCountdownForTest(countdown);
        SetCcmRunStateMetadataRollbackFsyncFailureForTest(fail_rollback_fsync);
    }

    ~ScopedMetadataPublishFsyncFailure() {
        SetCcmRunStateMetadataPublishFsyncFailureCountdownForTest(0);
        SetCcmRunStateMetadataRollbackFsyncFailureForTest(false);
    }

    ScopedMetadataPublishFsyncFailure(const ScopedMetadataPublishFsyncFailure&) = delete;
    ScopedMetadataPublishFsyncFailure& operator=(const ScopedMetadataPublishFsyncFailure&) = delete;
};

class ScopedMissingPasswdEntry final {
public:
    ScopedMissingPasswdEntry() {
        SetCcmRunStatePasswdLookupUnavailableForTest(true);
    }

    ~ScopedMissingPasswdEntry() {
        SetCcmRunStatePasswdLookupUnavailableForTest(false);
    }

    ScopedMissingPasswdEntry(const ScopedMissingPasswdEntry&) = delete;
    ScopedMissingPasswdEntry& operator=(const ScopedMissingPasswdEntry&) = delete;
};

std::vector<fs::path> ReservationOwnerFiles(const fs::path& root) {
    std::vector<fs::path> owners;
    for (const auto& entry : fs::directory_iterator(root / "ccm-id-locks")) {
        if (entry.path().extension() == ".owner") {
            owners.push_back(entry.path());
        }
    }
    return owners;
}

class ChildProcess final {
public:
    explicit ChildProcess(pid_t pid)
        : pid_(pid) {}

    ~ChildProcess() {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    [[nodiscard]] int Wait() {
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid_, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }
        pid_ = -1;
        return status;
    }

private:
    pid_t pid_;
};

struct ChildResult {
    int succeeded;
    int ccm_id;
};

struct MarkedChildResult {
    int succeeded;
    int ccm_id;
    pid_t marked_pid;
};

class OrphanProcessGuard final {
public:
    explicit OrphanProcessGuard(pid_t pid)
        : pid_(pid) {}

    ~OrphanProcessGuard() {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGKILL);
        }
    }

    void Release() noexcept {
        pid_ = -1;
    }

private:
    pid_t pid_;
};

std::pair<ScopedFd, ScopedFd> CreatePipe() {
    int descriptors[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
#else
    if (::pipe(descriptors) != 0) {
#endif
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    return {ScopedFd(descriptors[0]), ScopedFd(descriptors[1])};
}

bool WriteExactly(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::write(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool ReadExactly(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void WritePrivateFile(const fs::path& target, const std::string& contents) {
    const int fd = ::open(
        target.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + target.string());
    }
    ScopedFd owner(fd);
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod " + target.string());
    }
    if (!WriteExactly(fd, contents.data(), contents.size())) {
        throw std::system_error(errno, std::generic_category(), "write " + target.string());
    }
}

ClusterSpec OneNodeSpec(AlternatorTransport transport = AlternatorTransport::Http) {
    return ClusterSpec()
        .WithTopology(ClusterTopology::SingleDatacenter(1))
        .WithTransports({transport});
}

ScopedFd Bind(const std::string& host, std::uint16_t port) {
    ScopedFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket.Get() < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("invalid test bind address " + host);
    }
    if (::bind(
            socket.Get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        throw std::system_error(errno, std::generic_category(), "bind " + host);
    }
    return socket;
}

int FindAvailableId(const ClusterSpec& spec, bool include_jmx_port) {
    for (int id = 50; id <= 99; ++id) {
        if (CcmRunState::IsAddressRangeAvailable(spec, id, include_jmx_port)) {
            return id;
        }
    }
    return 0;
}

bool ProcessEnvironmentContains(pid_t pid, const std::string& expected) {
    std::ifstream input(
        fs::path("/proc") / std::to_string(pid) / "environ",
        std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string environment(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::size_t offset = 0;
    while (offset < environment.size()) {
        const auto end = environment.find('\0', offset);
        const auto size = (end == std::string::npos ? environment.size() : end) - offset;
        if (size == expected.size() && environment.compare(offset, size, expected) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return false;
}

bool ProcessIsZombie(pid_t pid) {
    std::ifstream input(fs::path("/proc") / std::to_string(pid) / "stat");
    if (!input) {
        return false;
    }
    std::string contents;
    std::getline(input, contents);
    const auto command_end = contents.rfind(')');
    return command_end != std::string::npos && command_end + 3 < contents.size() &&
           contents[command_end + 2] == 'Z';
}

void WaitForZombie(pid_t pid) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (ProcessIsZombie(pid)) {
            return;
        }
        ::usleep(10000);
    }
    throw std::runtime_error("child did not become zombie");
}

} // namespace

TEST(CcmRunState, BeginCompleteAndClosePublishAndRetireDurableState) {
    TemporaryDirectory temporary;
    auto state = CcmRunState::Open(temporary.Path(), {});
    const auto run_directory = state->RunDirectory();
    auto handle = state->BeginCluster("durable-cluster", OneNodeSpec(), false);

    const auto manifest = run_directory / "owned" / "durable-cluster.properties";
    const auto reservation = temporary.Path() / "ccm-id-locks" /
                             (std::to_string(handle.ccm_id) + ".owner");
    EXPECT_TRUE(fs::is_regular_file(run_directory / "OWNER"));
    EXPECT_TRUE(fs::is_regular_file(manifest));
    EXPECT_TRUE(fs::is_regular_file(reservation));
    EXPECT_THROW(state->Close(), std::runtime_error);

    state->CompleteCluster(handle);
    EXPECT_FALSE(fs::exists(manifest));
    EXPECT_FALSE(fs::exists(reservation));
    EXPECT_THROW(state->CompleteCluster(handle), std::invalid_argument);

    state->Close();
    EXPECT_FALSE(fs::exists(run_directory));
    EXPECT_NO_THROW(state->Close());
}

TEST(CcmRunState, ConcurrentProcessesUsingSameRootReserveDifferentIds) {
    TemporaryDirectory temporary;
    auto ready_pipe = CreatePipe();
    auto result_pipe = CreatePipe();
    auto release_pipe = CreatePipe();

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ready_pipe.second.Reset();
        result_pipe.first.Reset();
        release_pipe.second.Reset();
        (void)::alarm(30);

        char signal = '\0';
        if (!ReadExactly(ready_pipe.first.Get(), &signal, sizeof(signal))) {
            _exit(10);
        }
        try {
            auto state = CcmRunState::Open(temporary.Path(), {});
            auto handle = state->BeginCluster("child-cluster", OneNodeSpec(), false);
            const ChildResult result {1, handle.ccm_id};
            if (!WriteExactly(result_pipe.second.Get(), &result, sizeof(result))) {
                _exit(11);
            }
            if (!ReadExactly(release_pipe.first.Get(), &signal, sizeof(signal))) {
                _exit(12);
            }
            state->CompleteCluster(handle);
            state->Close();
            _exit(0);
        } catch (...) {
            const ChildResult result {0, 0};
            (void)WriteExactly(result_pipe.second.Get(), &result, sizeof(result));
            _exit(13);
        }
    }

    ChildProcess child(pid);
    ready_pipe.first.Reset();
    result_pipe.second.Reset();
    release_pipe.first.Reset();

    auto parent_state = CcmRunState::Open(temporary.Path(), {});
    auto parent_handle = parent_state->BeginCluster("parent-cluster", OneNodeSpec(), false);
    const char signal = 'x';
    ASSERT_TRUE(WriteExactly(ready_pipe.second.Get(), &signal, sizeof(signal)));
    ChildResult child_result {0, 0};
    const bool read_result = ReadExactly(result_pipe.first.Get(), &child_result, sizeof(child_result));
    const bool released = WriteExactly(release_pipe.second.Get(), &signal, sizeof(signal));
    const int child_status = child.Wait();

    parent_state->CompleteCluster(parent_handle);
    parent_state->Close();

    ASSERT_TRUE(read_result);
    ASSERT_TRUE(released);
    EXPECT_EQ(child_result.succeeded, 1);
    EXPECT_NE(child_result.ccm_id, parent_handle.ccm_id);
    ASSERT_TRUE(WIFEXITED(child_status));
    EXPECT_EQ(WEXITSTATUS(child_status), 0);
}

TEST(CcmRunState, HardExitedChildIsRecoveredAndItsAddressIdIsReused) {
    TemporaryDirectory temporary;
    auto result_pipe = CreatePipe();
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        result_pipe.first.Reset();
        (void)::alarm(30);
        try {
            auto state = CcmRunState::Open(temporary.Path(), {});
            auto handle = state->BeginCluster("abandoned-cluster", OneNodeSpec(), false);
            const ChildResult result {1, handle.ccm_id};
            if (!WriteExactly(result_pipe.second.Get(), &result, sizeof(result))) {
                _exit(20);
            }
            _exit(0);
        } catch (...) {
            const ChildResult result {0, 0};
            (void)WriteExactly(result_pipe.second.Get(), &result, sizeof(result));
            _exit(21);
        }
    }

    ChildProcess child(pid);
    result_pipe.second.Reset();
    ChildResult abandoned {0, 0};
    const bool read_result = ReadExactly(result_pipe.first.Get(), &abandoned, sizeof(abandoned));
    const int child_status = child.Wait();
    ASSERT_TRUE(read_result);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
    ASSERT_EQ(abandoned.succeeded, 1);

    int cleanup_calls = 0;
    std::string recovered_instance;
    int recovered_id = 0;
    auto recovered = CcmRunState::Open(
        temporary.Path(),
        [&](const fs::path& run_directory, const std::string& instance_id, int ccm_id) {
            EXPECT_TRUE(fs::exists(run_directory));
            ++cleanup_calls;
            recovered_instance = instance_id;
            recovered_id = ccm_id;
        });

    EXPECT_EQ(cleanup_calls, 1);
    EXPECT_EQ(recovered_instance, "abandoned-cluster");
    EXPECT_EQ(recovered_id, abandoned.ccm_id);
    auto replacement = recovered->BeginCluster("replacement-cluster", OneNodeSpec(), false);
    EXPECT_EQ(replacement.ccm_id, abandoned.ccm_id);
    recovered->CompleteCluster(replacement);
    recovered->Close();
}

TEST(CcmRunState, UnreapedMarkedZombieDoesNotBlockRecoveryOrIdReuse) {
    TemporaryDirectory temporary;
    auto result_pipe = CreatePipe();
    const pid_t zombie_pid = ::fork();
    ASSERT_GE(zombie_pid, 0);
    if (zombie_pid == 0) {
        result_pipe.first.Reset();
        (void)::alarm(30);
        try {
            auto state = CcmRunState::Open(temporary.Path(), {});
            auto abandoned = state->BeginCluster("zombie-cluster", OneNodeSpec(), false);
            const ChildResult result{1, abandoned.ccm_id};
            if (!WriteExactly(result_pipe.second.Get(), &result, sizeof(result)) ||
                ::setenv("SCYLLA_CCM_RUN_DIR", state->RunDirectory().c_str(), 1) != 0) {
                _exit(40);
            }
            ::execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
            _exit(41);
        } catch (...) {
            const ChildResult result{0, 0};
            (void)WriteExactly(result_pipe.second.Get(), &result, sizeof(result));
            _exit(42);
        }
    }
    ChildProcess zombie(zombie_pid);
    result_pipe.second.Reset();
    ChildResult abandoned{0, 0};
    ASSERT_TRUE(ReadExactly(result_pipe.first.Get(), &abandoned, sizeof(abandoned)));
    ASSERT_EQ(abandoned.succeeded, 1);

    const auto runs_directory = temporary.Path() / "runs";
    std::vector<fs::path> runs;
    for (const auto& entry : fs::directory_iterator(runs_directory)) {
        if (entry.is_directory()) {
            runs.push_back(entry.path());
        }
    }
    ASSERT_EQ(runs.size(), 1U);
    const auto stale_run = runs.front();
    const std::string marker = "SCYLLA_CCM_RUN_DIR=" + stale_run.string();
    for (int attempt = 0;
         attempt < 500 && !ProcessEnvironmentContains(zombie_pid, marker);
         ++attempt) {
        ::usleep(10000);
    }
    ASSERT_TRUE(ProcessEnvironmentContains(zombie_pid, marker));
    ASSERT_EQ(::kill(zombie_pid, SIGTERM), 0);
    WaitForZombie(zombie_pid);

    int cleanup_calls = 0;
    auto recovered = CcmRunState::Open(
        temporary.Path(),
        [&](const fs::path&, const std::string& instance_id, int ccm_id) {
            EXPECT_EQ(instance_id, "zombie-cluster");
            EXPECT_EQ(ccm_id, abandoned.ccm_id);
            ++cleanup_calls;
        });
    EXPECT_EQ(cleanup_calls, 1);
    EXPECT_FALSE(fs::exists(stale_run));
    auto replacement = recovered->BeginCluster("replacement-cluster", OneNodeSpec(), false);
    EXPECT_EQ(replacement.ccm_id, abandoned.ccm_id);
    recovered->CompleteCluster(replacement);
    recovered->Close();

    const int zombie_status = zombie.Wait();
    ASSERT_TRUE(WIFSIGNALED(zombie_status));
    EXPECT_EQ(WTERMSIG(zombie_status), SIGTERM);
}

TEST(CcmRunState, RecoveryTerminatesOnlyProcessBearingExactRunMarker) {
    TemporaryDirectory temporary;
    auto result_pipe = CreatePipe();
    const pid_t owner_pid = ::fork();
    ASSERT_GE(owner_pid, 0);
    if (owner_pid == 0) {
        result_pipe.first.Reset();
        (void)::alarm(30);
        try {
            auto state = CcmRunState::Open(temporary.Path(), {});
            auto handle = state->BeginCluster("marked-process-cluster", OneNodeSpec(), false);
            const auto run_marker = state->RunDirectory().string();
            const pid_t marked_pid = ::fork();
            if (marked_pid < 0) {
                _exit(30);
            }
            if (marked_pid == 0) {
                if (::setenv("SCYLLA_CCM_RUN_DIR", run_marker.c_str(), 1) != 0) {
                    _exit(31);
                }
                ::execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
                _exit(32);
            }
            const std::string expected = "SCYLLA_CCM_RUN_DIR=" + run_marker;
            for (int attempt = 0;
                 attempt < 100 && !ProcessEnvironmentContains(marked_pid, expected);
                 ++attempt) {
                ::usleep(10000);
            }
            const MarkedChildResult result{
                ProcessEnvironmentContains(marked_pid, expected) ? 1 : 0,
                handle.ccm_id,
                marked_pid,
            };
            (void)WriteExactly(result_pipe.second.Get(), &result, sizeof(result));
            _exit(result.succeeded == 1 ? 0 : 33);
        } catch (...) {
            const MarkedChildResult result{0, 0, -1};
            (void)WriteExactly(result_pipe.second.Get(), &result, sizeof(result));
            _exit(34);
        }
    }

    ChildProcess owner(owner_pid);
    result_pipe.second.Reset();
    MarkedChildResult abandoned{0, 0, -1};
    const bool read_result = ReadExactly(result_pipe.first.Get(), &abandoned, sizeof(abandoned));
    const int owner_status = owner.Wait();
    ASSERT_TRUE(read_result);
    ASSERT_TRUE(WIFEXITED(owner_status));
    ASSERT_EQ(WEXITSTATUS(owner_status), 0);
    ASSERT_EQ(abandoned.succeeded, 1);
    ASSERT_GT(abandoned.marked_pid, 1);
    OrphanProcessGuard marked_process(abandoned.marked_pid);

    int cleanup_calls = 0;
    auto recovered = CcmRunState::Open(
        temporary.Path(),
        [&](const fs::path&, const std::string& instance_id, int ccm_id) {
            EXPECT_EQ(instance_id, "marked-process-cluster");
            EXPECT_EQ(ccm_id, abandoned.ccm_id);
            ++cleanup_calls;
        });
    EXPECT_EQ(cleanup_calls, 1);

    bool process_gone = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::kill(abandoned.marked_pid, 0) != 0 && errno == ESRCH) {
            process_gone = true;
            break;
        }
        ::usleep(10000);
    }
    EXPECT_TRUE(process_gone);
    if (process_gone) {
        marked_process.Release();
    }
    recovered->Close();
}

TEST(CcmRunState, MalformedStaleRunAndReservationRemainQuarantined) {
    TemporaryDirectory temporary;
    auto bootstrap = CcmRunState::Open(temporary.Path(), {});
    bootstrap->Close();

    const std::string stale_name = "ccm-runtime.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto stale_run = temporary.Path() / "runs" / stale_name;
    const auto owned = stale_run / "owned";
    ASSERT_TRUE(fs::create_directory(stale_run));
    ASSERT_TRUE(fs::create_directory(owned));
    fs::permissions(stale_run, fs::perms::owner_all, fs::perm_options::replace);
    fs::permissions(owned, fs::perms::owner_all, fs::perm_options::replace);
    WritePrivateFile(stale_run / "OWNER", "not-valid-owner-metadata\n");
    WritePrivateFile(
        owned / "quarantined-cluster.properties",
        "format=1\ninstance_id=quarantined-cluster\nccm_id=1\n");
    const auto stale_reservation = temporary.Path() / "ccm-id-locks" / "1.owner";
    WritePrivateFile(stale_reservation, stale_name + "\n");

    int cleanup_calls = 0;
    auto next = CcmRunState::Open(
        temporary.Path(),
        [&](const fs::path&, const std::string&, int) { ++cleanup_calls; });
    EXPECT_EQ(cleanup_calls, 0);
    EXPECT_TRUE(fs::exists(stale_run));
    EXPECT_TRUE(fs::exists(stale_reservation));

    auto handle = next->BeginCluster("healthy-cluster", OneNodeSpec(), false);
    EXPECT_NE(handle.ccm_id, 1);
    next->CompleteCluster(handle);
    next->Close();
    EXPECT_TRUE(fs::exists(stale_run));
    EXPECT_TRUE(fs::exists(stale_reservation));
}

TEST(CcmRunState, RejectsBroadAndSymlinkedRoots) {
    EXPECT_THROW((void)CcmRunState::Open("/tmp", {}), std::runtime_error);

    TemporaryDirectory temporary;
    const auto environment_home = temporary.Path() / "environment-home";
    ScopedEnvironment home("HOME", environment_home.string());
    EXPECT_THROW((void)CcmRunState::Open(environment_home, {}), std::runtime_error);
    EXPECT_FALSE(fs::exists(environment_home));

    const auto real_root = temporary.Path() / "real-root";
    const auto linked_root = temporary.Path() / "linked-root";
    ASSERT_TRUE(fs::create_directory(real_root));
    ASSERT_NO_THROW(fs::create_directory_symlink(real_root, linked_root));
    EXPECT_THROW((void)CcmRunState::Open(linked_root, {}), std::runtime_error);
}

TEST(CcmRunState, MissingPasswdEntryUsesEnvironmentHomeWithoutBlockingSafeRoot) {
    TemporaryDirectory temporary;
    const auto environment_home = temporary.Path() / "environment-home";
    ScopedEnvironment home("HOME", environment_home.string());
    ScopedMissingPasswdEntry missing_passwd_entry;

    auto state = CcmRunState::Open(temporary.Path(), {});
    state->Close();
    EXPECT_THROW((void)CcmRunState::Open(environment_home, {}), std::runtime_error);
}

TEST(CcmRunState, AddressAvailabilityHonorsTransportAndJmxPorts) {
    const auto http = OneNodeSpec(AlternatorTransport::Http);
    const auto https = OneNodeSpec(AlternatorTransport::Https);
    const int ccm_id = FindAvailableId(
        ClusterSpec()
            .WithTopology(ClusterTopology::SingleDatacenter(1))
            .WithTransports({AlternatorTransport::Http, AlternatorTransport::Https}),
        true);
    ASSERT_NE(ccm_id, 0);
    const std::string host = "127.0." + std::to_string(ccm_id) + ".1";

    EXPECT_FALSE(CcmRunState::IsAddressRangeAvailable(http, 0, false));
    EXPECT_FALSE(CcmRunState::IsAddressRangeAvailable(http, 100, false));
    EXPECT_TRUE(CcmRunState::IsAddressRangeAvailable(http, ccm_id, false));
    EXPECT_TRUE(CcmRunState::IsAddressRangeAvailable(https, ccm_id, true));

    {
        auto occupied_http = Bind(host, 8080);
        EXPECT_FALSE(CcmRunState::IsAddressRangeAvailable(http, ccm_id, false));
        EXPECT_TRUE(CcmRunState::IsAddressRangeAvailable(https, ccm_id, false));
    }
    {
        auto occupied_jmx = Bind(host, 7199);
        EXPECT_TRUE(CcmRunState::IsAddressRangeAvailable(http, ccm_id, false));
        EXPECT_FALSE(CcmRunState::IsAddressRangeAvailable(http, ccm_id, true));
    }
}

TEST(CcmRunState, ReservationPublishSyncFailureDoesNotLeaveAnOrphanOwner) {
    TemporaryDirectory temporary;
    auto state = CcmRunState::Open(temporary.Path(), {});

    CcmRunState::ClusterHandle handle;
    {
        ScopedMetadataPublishFsyncFailure failure(1);
        ASSERT_NO_THROW(
            handle = state->BeginCluster("reservation-retry-cluster", OneNodeSpec(), false));
    }

    const auto owners = ReservationOwnerFiles(temporary.Path());
    ASSERT_EQ(owners.size(), 1U);
    EXPECT_EQ(
        owners.front().filename(),
        std::to_string(handle.ccm_id) + ".owner");

    state->CompleteCluster(handle);
    EXPECT_TRUE(ReservationOwnerFiles(temporary.Path()).empty());
    state->Close();
}

TEST(CcmRunState, ManifestPublishSyncFailureRollsBackManifestAndReservation) {
    TemporaryDirectory temporary;
    auto state = CcmRunState::Open(temporary.Path(), {});
    const auto run_directory = state->RunDirectory();
    const auto failed_manifest =
        run_directory / "owned" / "failed-manifest-cluster.properties";

    {
        ScopedMetadataPublishFsyncFailure failure(3);
        EXPECT_THROW(
            (void)state->BeginCluster(
                "failed-manifest-cluster", OneNodeSpec(), false),
            std::runtime_error);
    }

    EXPECT_FALSE(fs::exists(failed_manifest));
    EXPECT_TRUE(ReservationOwnerFiles(temporary.Path()).empty());

    auto replacement =
        state->BeginCluster("replacement-cluster", OneNodeSpec(), false);
    state->CompleteCluster(replacement);
    state->Close();
    EXPECT_FALSE(fs::exists(run_directory));
}

TEST(CcmRunState, StagingCleanupSyncFailureKeepsCommittedMetadataSuccessful) {
    TemporaryDirectory temporary;
    auto state = CcmRunState::Open(temporary.Path(), {});

    CcmRunState::ClusterHandle handle;
    {
        ScopedMetadataPublishFsyncFailure failure(2);
        EXPECT_NO_THROW(
            handle = state->BeginCluster(
                "committed-metadata-cluster", OneNodeSpec(), false));
    }

    EXPECT_EQ(ReservationOwnerFiles(temporary.Path()).size(), 1U);
    EXPECT_TRUE(fs::is_regular_file(
        state->RunDirectory() / "owned" /
        "committed-metadata-cluster.properties"));
    state->CompleteCluster(handle);
    state->Close();
}

TEST(CcmRunState, UncertainManifestRollbackQuarantinesUntilNextProcessRecovery) {
    TemporaryDirectory temporary;
    const pid_t child_pid = ::fork();
    ASSERT_GE(child_pid, 0);
    if (child_pid == 0) {
        (void)::alarm(30);
        try {
            auto state = CcmRunState::Open(temporary.Path(), {});
            {
                ScopedMetadataPublishFsyncFailure failure(3, true);
                try {
                    (void)state->BeginCluster(
                        "uncertain-manifest-cluster", OneNodeSpec(), false);
                    _exit(50);
                } catch (const std::runtime_error&) {
                }
            }
            try {
                (void)state->BeginCluster("must-remain-blocked", OneNodeSpec(), false);
                _exit(51);
            } catch (const std::runtime_error&) {
            }
            try {
                state->Close();
                _exit(52);
            } catch (const std::runtime_error&) {
            }
            _exit(0);
        } catch (...) {
            _exit(53);
        }
    }

    ChildProcess child(child_pid);
    const int child_status = child.Wait();
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);

    std::vector<fs::path> stale_runs;
    for (const auto& entry : fs::directory_iterator(temporary.Path() / "runs")) {
        if (entry.is_directory()) {
            stale_runs.push_back(entry.path());
        }
    }
    ASSERT_EQ(stale_runs.size(), 1U);
    const auto stale_run = stale_runs.front();
    EXPECT_TRUE(fs::is_regular_file(
        stale_run / "owned" / "uncertain-manifest-cluster.properties"));
    const auto owners = ReservationOwnerFiles(temporary.Path());
    ASSERT_EQ(owners.size(), 1U);
    const int quarantined_id = std::stoi(owners.front().stem().string());

    int cleanup_calls = 0;
    auto recovered = CcmRunState::Open(
        temporary.Path(),
        [&](const fs::path&, const std::string& instance_id, int ccm_id) {
            EXPECT_EQ(instance_id, "uncertain-manifest-cluster");
            EXPECT_EQ(ccm_id, quarantined_id);
            ++cleanup_calls;
        });
    EXPECT_EQ(cleanup_calls, 1);
    EXPECT_FALSE(fs::exists(stale_run));
    EXPECT_TRUE(ReservationOwnerFiles(temporary.Path()).empty());

    auto replacement =
        recovered->BeginCluster("replacement-cluster", OneNodeSpec(), false);
    EXPECT_EQ(replacement.ccm_id, quarantined_id);
    recovered->CompleteCluster(replacement);
    recovered->Close();
}
