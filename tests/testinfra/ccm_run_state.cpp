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

#include "test_cluster_internal.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace scylladb::alternator::testinfra {
namespace {

namespace fs = std::filesystem;

constexpr int kMetadataFormat = 1;
constexpr std::uintmax_t kMaximumMetadataBytes = 4096;
constexpr std::size_t kMaximumProcFileBytes = 1024 * 1024;
constexpr int kMinimumCcmId = 1;
constexpr int kMaximumCcmId = 99;
constexpr int kStoragePort = 7000;
constexpr int kJmxPort = 7199;
constexpr int kCqlPort = 9042;
constexpr int kPrometheusPort = 9180;
constexpr int kApiPort = 10000;
constexpr int kShardAwareCqlPort = 19042;
constexpr int kAlternatorHttpPort = 8080;
constexpr int kAlternatorHttpsPort = 8043;
constexpr auto kTerminateGrace = std::chrono::seconds(2);
constexpr auto kKillGrace = std::chrono::seconds(5);
constexpr auto kProcessPollPeriod = std::chrono::milliseconds(50);

std::mutex scan_mutex;
std::atomic<std::uint32_t> metadata_publish_fsync_failure_countdown{0};
std::atomic<bool> metadata_rollback_fsync_failure{false};
std::atomic<bool> passwd_lookup_unavailable{false};

class MetadataPublicationError final : public std::runtime_error {
public:
    MetadataPublicationError(std::string message, bool rollback_durable)
        : std::runtime_error(std::move(message))
        , rollback_durable_(rollback_durable) {}

    [[nodiscard]] bool RollbackDurable() const noexcept {
        return rollback_durable_;
    }

private:
    bool rollback_durable_;
};

void MaybeFailMetadataPublishFsyncForTest() {
    auto remaining = metadata_publish_fsync_failure_countdown.load(std::memory_order_relaxed);
    while (remaining != 0) {
        if (metadata_publish_fsync_failure_countdown.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_relaxed)) {
            if (remaining == 1) {
                throw std::runtime_error(
                    "injected CCM metadata publication directory-sync failure");
            }
            return;
        }
    }
}

void MaybeFailMetadataRollbackFsyncForTest() {
    if (metadata_rollback_fsync_failure.exchange(false, std::memory_order_relaxed)) {
        throw std::runtime_error(
            "injected CCM metadata rollback directory-sync failure");
    }
}

void AppendFailure(std::string& failure, const std::string& additional) {
    if (failure.empty()) {
        failure = additional;
    } else {
        failure += "; additionally " + additional;
    }
}

class UniqueFd final {
public:
    UniqueFd() = default;

    explicit UniqueFd(int fd)
        : fd_(fd) {
    }

    ~UniqueFd() {
        Reset();
    }

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(other.Release()) {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

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
            // Retrying close after EINTR can close an unrelated reused descriptor on Linux.
            (void)::close(fd_);
        }
        fd_ = replacement;
    }

private:
    int fd_ = -1;
};

[[noreturn]] void ThrowErrno(const std::string& operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}

[[noreturn]] void ThrowFilesystemError(const std::string& operation, const std::error_code& error) {
    throw std::system_error(error, operation);
}

bool IsMissingError(const std::system_error& error) {
    return error.code().value() == ENOENT || error.code().value() == ESRCH;
}

std::string CurrentExceptionMessage() {
    try {
        throw;
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "unknown failure";
    }
}

std::optional<struct stat> ReadStat(const fs::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) == 0) {
        return status;
    }
    if (errno == ENOENT) {
        return std::nullopt;
    }
    ThrowErrno("cannot inspect " + path.string());
}

void RequireCurrentOwner(const struct stat& status, const fs::path& path, const char* description) {
    if (status.st_uid != ::geteuid()) {
        throw std::runtime_error(
            std::string(description) + " is not owned by current user: " + path.string());
    }
}

void RequirePrivateMode(const struct stat& status, const fs::path& path, const char* description) {
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error(
            std::string(description) + " is accessible by group or other users: " + path.string());
    }
}

void ValidateOwnedDirectory(
    const fs::path& directory,
    const char* description,
    bool require_private_mode) {
    const auto status = ReadStat(directory);
    if (!status || !S_ISDIR(status->st_mode)) {
        throw std::runtime_error(
            std::string(description) + " is not a directory: " + directory.string());
    }
    RequireCurrentOwner(*status, directory, description);
    if (require_private_mode) {
        RequirePrivateMode(*status, directory, description);
    }

    std::error_code error;
    const auto canonical = fs::canonical(directory, error);
    if (error) {
        ThrowFilesystemError("cannot resolve " + directory.string(), error);
    }
    if (canonical != directory.lexically_normal()) {
        throw std::runtime_error(
            std::string(description) + " traverses a symbolic link: " + directory.string());
    }
}

void MakeDirectoryPrivate(const fs::path& directory, const char* description) {
    const int fd = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        ThrowErrno("cannot open " + std::string(description) + " " + directory.string());
    }
    UniqueFd owner(fd);
    struct stat status {};
    if (::fstat(owner.Get(), &status) != 0) {
        ThrowErrno("cannot inspect " + std::string(description) + " " + directory.string());
    }
    if (!S_ISDIR(status.st_mode)) {
        throw std::runtime_error(
            std::string(description) + " is not a directory: " + directory.string());
    }
    RequireCurrentOwner(status, directory, description);
    if (::fchmod(owner.Get(), S_IRWXU) != 0) {
        ThrowErrno("cannot make " + std::string(description) + " private at " + directory.string());
    }
    if (::fstat(owner.Get(), &status) != 0) {
        ThrowErrno("cannot verify permissions for " + directory.string());
    }
    RequirePrivateMode(status, directory, description);
}

void FsyncDirectory(const fs::path& directory) {
    const int fd = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        ThrowErrno("cannot open directory for synchronization " + directory.string());
    }
    UniqueFd owner(fd);
    if (::fsync(owner.Get()) != 0) {
        ThrowErrno("cannot synchronize directory " + directory.string());
    }
}

std::vector<fs::path> ListDirectory(const fs::path& directory) {
    std::vector<fs::path> entries;
    std::error_code error;
    fs::directory_iterator iterator(directory, error);
    if (error) {
        ThrowFilesystemError("cannot enumerate " + directory.string(), error);
    }
    const fs::directory_iterator end;
    while (iterator != end) {
        entries.push_back(iterator->path());
        iterator.increment(error);
        if (error) {
            ThrowFilesystemError("cannot enumerate " + directory.string(), error);
        }
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

std::string ReadFromFd(int fd, const fs::path& path, std::size_t maximum_size) {
    std::string value;
    std::array<char, 4096> buffer {};
    while (true) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            if (value.size() + static_cast<std::size_t>(count) > maximum_size) {
                throw std::runtime_error("file is too large: " + path.string());
            }
            value.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            return value;
        }
        if (errno != EINTR) {
            ThrowErrno("cannot read " + path.string());
        }
    }
}

std::string ReadRawFile(const fs::path& path, std::size_t maximum_size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        ThrowErrno("cannot open " + path.string());
    }
    UniqueFd owner(fd);
    return ReadFromFd(owner.Get(), path, maximum_size);
}

std::string ReadMetadataFile(const fs::path& path, const char* description) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        ThrowErrno("cannot open " + std::string(description) + " " + path.string());
    }
    UniqueFd owner(fd);
    struct stat status {};
    if (::fstat(owner.Get(), &status) != 0) {
        ThrowErrno("cannot inspect " + std::string(description) + " " + path.string());
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > kMaximumMetadataBytes) {
        throw std::runtime_error(std::string("unsafe ") + description + " at " + path.string());
    }
    RequireCurrentOwner(status, path, description);
    RequirePrivateMode(status, path, description);

    auto value = ReadFromFd(
        owner.Get(),
        path,
        static_cast<std::size_t>(kMaximumMetadataBytes));
    if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
            return character > 0x7fU || character == '\0';
        })) {
        throw std::runtime_error(std::string("non-ASCII ") + description + " at " + path.string());
    }
    return value;
}

void WriteAll(int fd, const std::string& value, const fs::path& path) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = ::write(fd, value.data() + offset, value.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            ThrowErrno("cannot write metadata " + path.string());
        }
    }
}

std::string TemporaryMetadataName(const fs::path& target, std::uint64_t attempt) {
    return ".owner-" + target.filename().string() + "-" + std::to_string(::getpid()) + "-" +
           std::to_string(attempt) + ".tmp";
}

void WriteExclusive(const fs::path& target, const std::string& contents) {
    if (contents.size() > kMaximumMetadataBytes) {
        throw std::invalid_argument("CCM metadata is too large for " + target.string());
    }
    const fs::path parent = target.parent_path();
    fs::path temporary;
    UniqueFd output;
    static std::atomic<std::uint64_t> sequence {0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        temporary = parent / TemporaryMetadataName(target, ++sequence);
        const int fd = ::open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            output.Reset(fd);
            break;
        }
        if (errno != EEXIST) {
            ThrowErrno("cannot create metadata staging file " + temporary.string());
        }
    }
    if (output.Get() < 0) {
        throw std::runtime_error("cannot allocate metadata staging file beneath " + parent.string());
    }

    bool published = false;
    bool publication_committed = false;
    bool publication_linked = false;
    std::string failure;
    try {
        if (::fchmod(output.Get(), S_IRUSR | S_IWUSR) != 0) {
            ThrowErrno("cannot protect metadata staging file " + temporary.string());
        }
        WriteAll(output.Get(), contents, temporary);
        if (::fsync(output.Get()) != 0) {
            ThrowErrno("cannot synchronize metadata staging file " + temporary.string());
        }
        output.Reset();
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            ThrowErrno("cannot publish metadata " + target.string());
        }
        published = true;
        publication_linked = true;
        MaybeFailMetadataPublishFsyncForTest();
        FsyncDirectory(parent);
        publication_committed = true;
    } catch (...) {
        failure = CurrentExceptionMessage();
    }

    if (!publication_committed && !failure.empty() && published) {
        bool rollback_durable = false;
        if (::unlink(target.c_str()) != 0 && errno != ENOENT) {
            AppendFailure(
                failure,
                std::system_error(
                    errno,
                    std::generic_category(),
                    "cannot roll back published metadata " + target.string())
                    .what());
        } else {
            published = false;
            try {
                MaybeFailMetadataRollbackFsyncForTest();
                FsyncDirectory(parent);
                rollback_durable = true;
            } catch (...) {
                AppendFailure(
                    failure,
                    "cannot synchronize published metadata rollback: " +
                        CurrentExceptionMessage());
            }
        }
        if (!rollback_durable) {
            if (!published) {
                if (::link(temporary.c_str(), target.c_str()) == 0 || errno == EEXIST) {
                    published = true;
                    try {
                        FsyncDirectory(parent);
                    } catch (...) {
                        AppendFailure(
                            failure,
                            "cannot synchronize retained ambiguous metadata: " +
                                CurrentExceptionMessage());
                    }
                } else {
                    AppendFailure(
                        failure,
                        std::system_error(
                            errno,
                            std::generic_category(),
                            "cannot retain ambiguous metadata " + target.string())
                            .what());
                }
            }
            std::cerr << "Leaving recoverable CCM metadata staging file at " << temporary << '\n';
            throw MetadataPublicationError(failure, false);
        }
    }

    if (::unlink(temporary.c_str()) != 0 && errno != ENOENT) {
        if (publication_committed) {
            std::cerr << "Leaving recoverable CCM metadata staging file at " << temporary << '\n';
        } else {
            AppendFailure(
                failure,
                std::system_error(
                    errno,
                    std::generic_category(),
                    "cannot remove metadata staging file " + temporary.string())
                    .what());
        }
    } else {
        try {
            MaybeFailMetadataPublishFsyncForTest();
            FsyncDirectory(parent);
        } catch (...) {
            if (publication_committed) {
                std::cerr << "Published CCM metadata at " << target
                          << " but could not synchronize staging cleanup: "
                          << CurrentExceptionMessage() << '\n';
            } else {
                AppendFailure(failure, CurrentExceptionMessage());
            }
        }
    }
    if (!failure.empty()) {
        if (publication_linked && !publication_committed) {
            throw MetadataPublicationError(failure, true);
        }
        throw std::runtime_error(failure);
    }
}

int OpenLockFile(const fs::path& path) {
    const int fd = ::open(
        path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (fd < 0) {
        ThrowErrno("cannot open CCM lock file " + path.string());
    }
    UniqueFd owner(fd);
    struct stat status {};
    if (::fstat(owner.Get(), &status) != 0) {
        ThrowErrno("cannot inspect CCM lock file " + path.string());
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        throw std::runtime_error("unsafe CCM lock file at " + path.string());
    }
    RequireCurrentOwner(status, path, "CCM lock file");
    if (::fchmod(owner.Get(), S_IRUSR | S_IWUSR) != 0) {
        ThrowErrno("cannot protect CCM lock file " + path.string());
    }
    return owner.Release();
}

void LockExclusive(int fd, const fs::path& path) {
    while (::flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            ThrowErrno("cannot lock " + path.string());
        }
    }
}

bool TryLockExclusive(int fd, const fs::path& path) {
    while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return false;
        }
        if (errno != EINTR) {
            ThrowErrno("cannot lock " + path.string());
        }
    }
    return true;
}

template <typename Operation>
auto WithScanLock(const fs::path& path, Operation&& operation) -> decltype(operation()) {
    std::lock_guard<std::mutex> process_lock(scan_mutex);
    UniqueFd lock(OpenLockFile(path));
    LockExclusive(lock.Get(), path);
    return operation();
}

bool IsPathPrefix(const fs::path& prefix, const fs::path& path) {
    auto prefix_part = prefix.begin();
    auto path_part = path.begin();
    for (; prefix_part != prefix.end(); ++prefix_part, ++path_part) {
        if (path_part == path.end() || *prefix_part != *path_part) {
            return false;
        }
    }
    return true;
}

std::string TrimAsciiWhitespace(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::vector<fs::path> CurrentHomeDirectories() {
    std::vector<fs::path> homes;
    const auto add_home = [&](const char* value) {
        if (value == nullptr || value[0] == '\0') {
            return;
        }
        const fs::path home(value);
        if (!home.is_absolute()) {
            return;
        }
        const auto normalized = home.lexically_normal();
        if (std::find(homes.begin(), homes.end(), normalized) == homes.end()) {
            homes.push_back(normalized);
        }
    };

    const struct passwd* entry = passwd_lookup_unavailable.load(std::memory_order_relaxed)
        ? nullptr
        : ::getpwuid(::geteuid());
    if (entry != nullptr) {
        add_home(entry->pw_dir);
    }
    add_home(std::getenv("HOME"));
    return homes;
}

void RejectUnsafeRoot(const fs::path& root) {
    const fs::path filesystem_root = root.root_path();
    const fs::path temporary_root = fs::path("/tmp");
    const fs::path project = fs::current_path().lexically_normal();
    const fs::path build = project / "build";
    const auto homes = CurrentHomeDirectories();
    const bool is_home = std::find(homes.begin(), homes.end(), root) != homes.end();
    if (root.empty() || filesystem_root.empty() || root == filesystem_root ||
        root.parent_path().empty() || root.parent_path() == filesystem_root ||
        root == temporary_root || root == fs::path("/var/tmp") || root == fs::path("/dev/shm") ||
        is_home || IsPathPrefix(root, project) || IsPathPrefix(build, root)) {
        throw std::runtime_error("refusing unsafe CCM root: " + root.string());
    }
}

void RejectSymbolicLinkComponents(const fs::path& path) {
    for (fs::path current = path; !current.empty(); current = current.parent_path()) {
        const auto status = ReadStat(current);
        if (status && S_ISLNK(status->st_mode)) {
            throw std::runtime_error("CCM root must not traverse symbolic links: " + path.string());
        }
        if (current == current.root_path()) {
            break;
        }
    }
}

fs::path PrepareRoot(const fs::path& requested) {
    if (requested.empty()) {
        throw std::invalid_argument("CCM root cannot be empty");
    }
    const fs::path normalized = fs::absolute(requested).lexically_normal();
    RejectUnsafeRoot(normalized);
    RejectSymbolicLinkComponents(normalized);
    std::error_code error;
    fs::create_directories(normalized, error);
    if (error) {
        ThrowFilesystemError("cannot create CCM root " + normalized.string(), error);
    }
    RejectSymbolicLinkComponents(normalized);
    MakeDirectoryPrivate(normalized, "CCM root");
    ValidateOwnedDirectory(normalized, "CCM root", true);
    return normalized;
}

fs::path PrepareChildDirectory(const fs::path& root, const std::string& name) {
    const fs::path child = root / name;
    if (::mkdir(child.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        ThrowErrno("cannot create CCM directory " + child.string());
    }
    MakeDirectoryPrivate(child, "CCM child directory");
    ValidateOwnedDirectory(child, "CCM child directory", true);
    FsyncDirectory(root);
    return child;
}

bool IsLowerHex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

bool IsAsciiDigit(char character) {
    return character >= '0' && character <= '9';
}

bool IsAsciiAlpha(char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
}

bool IsAsciiAlphanumeric(char character) {
    return IsAsciiAlpha(character) || IsAsciiDigit(character);
}

bool IsRunName(const std::string& name) {
    constexpr const char* prefix = "ccm-runtime.";
    constexpr std::size_t prefix_length = 12;
    return name.size() == prefix_length + 32 && name.compare(0, prefix_length, prefix) == 0 &&
           std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix_length), name.end(), IsLowerHex);
}

void ValidateInstanceId(const std::string& instance_id) {
    if (instance_id.empty() || instance_id.size() > 128 ||
        !IsAsciiAlphanumeric(instance_id.front()) ||
        !std::all_of(instance_id.begin(), instance_id.end(), [](char character) {
            return IsAsciiAlphanumeric(character) || character == '-';
        })) {
        throw std::invalid_argument("unsafe CCM cluster instance ID: " + instance_id);
    }
}

void ValidateRunDirectory(const fs::path& run, const fs::path& runs_directory) {
    if (run.parent_path() != runs_directory || !IsRunName(run.filename().string())) {
        throw std::runtime_error("unsafe CCM run path: " + run.string());
    }
    ValidateOwnedDirectory(run, "CCM run directory", true);
}

std::string RandomRunName() {
    std::array<unsigned char, 16> bytes {};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            ThrowErrno("cannot obtain randomness for CCM run name");
        }
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "ccm-runtime.";
    result.reserve(result.size() + bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

struct ProcessStat {
    char state;
    std::uint64_t start_ticks;
};

bool IsProcessActive(const ProcessStat& process) {
    return process.state != 'Z' && process.state != 'X' && process.state != 'x';
}

ProcessStat ReadProcessStat(pid_t pid) {
    const auto path = fs::path("/proc") / std::to_string(pid) / "stat";
    const auto contents = ReadRawFile(path, kMaximumProcFileBytes);
    const auto command_end = contents.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= contents.size()) {
        throw std::runtime_error("cannot parse process identity for PID " + std::to_string(pid));
    }
    std::istringstream fields(contents.substr(command_end + 2));
    std::string field;
    char state = '\0';
    for (int index = 0; index <= 19; ++index) {
        if (!(fields >> field)) {
            throw std::runtime_error("cannot parse process identity for PID " + std::to_string(pid));
        }
        if (index == 0) {
            if (field.size() != 1) {
                throw std::runtime_error("cannot parse process state for PID " + std::to_string(pid));
            }
            state = field.front();
        }
    }
    std::uint64_t ticks = 0;
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), ticks);
    if (parsed.ec != std::errc() || parsed.ptr != field.data() + field.size() || ticks == 0) {
        throw std::runtime_error("cannot parse process identity for PID " + std::to_string(pid));
    }
    if (state == '\0') {
        throw std::runtime_error("cannot parse process state for PID " + std::to_string(pid));
    }
    return {state, ticks};
}

std::uint64_t ReadProcessStartTicks(pid_t pid) {
    return ReadProcessStat(pid).start_ticks;
}

std::string CurrentBootId() {
    auto value = TrimAsciiWhitespace(
        ReadRawFile("/proc/sys/kernel/random/boot_id", kMaximumMetadataBytes));
    if (value.size() != 36 ||
        !std::all_of(value.begin(), value.end(), [](char character) {
            return IsAsciiDigit(character) ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F') || character == '-';
        })) {
        throw std::runtime_error("cannot parse current Linux boot ID");
    }
    return value;
}

template <typename Integer>
Integer ParsePositiveInteger(const std::string& value, const fs::path& source, const char* field) {
    Integer parsed {};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        parsed <= 0) {
        throw std::runtime_error(
            "malformed " + std::string(field) + " in " + source.string());
    }
    return parsed;
}

std::map<std::string, std::string> ParseProperties(
    const fs::path& path,
    const std::set<std::string>& expected_keys) {
    const std::string contents = ReadMetadataFile(path, "CCM metadata");
    if (contents.empty() || contents.back() != '\n') {
        throw std::runtime_error("malformed CCM metadata at " + path.string());
    }
    std::map<std::string, std::string> values;
    std::size_t offset = 0;
    while (offset + 1 <= contents.size()) {
        const auto newline = contents.find('\n', offset);
        if (newline == std::string::npos) {
            break;
        }
        if (newline == offset && newline + 1 == contents.size()) {
            break;
        }
        const auto line = contents.substr(offset, newline - offset);
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 == line.size()) {
            throw std::runtime_error("malformed CCM metadata at " + path.string());
        }
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (expected_keys.count(key) == 0 || !values.emplace(key, value).second) {
            throw std::runtime_error("malformed CCM metadata at " + path.string());
        }
        offset = newline + 1;
        if (offset == contents.size()) {
            break;
        }
    }
    if (offset != contents.size() || values.size() != expected_keys.size()) {
        throw std::runtime_error("incomplete CCM metadata at " + path.string());
    }
    for (const auto& key : expected_keys) {
        if (values.count(key) == 0) {
            throw std::runtime_error("incomplete CCM metadata at " + path.string());
        }
    }
    const auto format = ParsePositiveInteger<int>(values.at("format"), path, "metadata format");
    if (format != kMetadataFormat) {
        throw std::runtime_error("unsupported CCM metadata format at " + path.string());
    }
    return values;
}

int ParseCcmId(const std::string& value, const fs::path& source) {
    const int id = ParsePositiveInteger<int>(value, source, "CCM ID");
    if (id < kMinimumCcmId || id > kMaximumCcmId || std::to_string(id) != value) {
        throw std::runtime_error("malformed CCM ID at " + source.string());
    }
    return id;
}

struct OwnerIdentity {
    pid_t pid;
    std::uint64_t start_ticks;
    std::string boot_id;
};

OwnerIdentity ReadOwner(const fs::path& owner_path) {
    const auto values = ParseProperties(
        owner_path,
        {"format", "pid", "start_ticks", "boot_id"});
    const auto parsed_pid = ParsePositiveInteger<std::uint64_t>(values.at("pid"), owner_path, "PID");
    if (parsed_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        throw std::runtime_error("malformed PID in " + owner_path.string());
    }
    const auto ticks = ParsePositiveInteger<std::uint64_t>(
        values.at("start_ticks"), owner_path, "process start ticks");
    const auto& boot_id = values.at("boot_id");
    if (boot_id.size() != 36 ||
        !std::all_of(boot_id.begin(), boot_id.end(), [](char character) {
            return IsAsciiDigit(character) ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F') || character == '-';
        })) {
        throw std::runtime_error("malformed boot ID in " + owner_path.string());
    }
    return {static_cast<pid_t>(parsed_pid), ticks, boot_id};
}

bool IsOwnerActive(const OwnerIdentity& owner) {
    if (owner.boot_id != CurrentBootId()) {
        return false;
    }
    try {
        const auto process = ReadProcessStat(owner.pid);
        return IsProcessActive(process) && process.start_ticks == owner.start_ticks;
    } catch (const std::system_error& error) {
        if (IsMissingError(error)) {
            return false;
        }
        throw;
    }
}

struct ManifestFile {
    fs::path path;
    std::string instance_id;
    int ccm_id;
};

bool IsMetadataStagingName(const std::string& name) {
    return name.size() > 11 && name.size() <= 255 && name.compare(0, 7, ".owner-") == 0 &&
           name.compare(name.size() - 4, 4, ".tmp") == 0 &&
           std::all_of(name.begin(), name.end(), [](char character) {
               return IsAsciiAlphanumeric(character) || character == '.' || character == '_' ||
                      character == '-';
           });
}

void ValidateStagingFile(const fs::path& path) {
    (void)ReadMetadataFile(path, "CCM metadata staging file");
}

void RemoveRegularFile(const fs::path& path, const char* description) {
    const auto status = ReadStat(path);
    if (!status) {
        return;
    }
    if (!S_ISREG(status->st_mode)) {
        throw std::runtime_error(std::string("unsafe ") + description + " at " + path.string());
    }
    RequireCurrentOwner(*status, path, description);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        ThrowErrno("cannot remove " + path.string());
    }
    FsyncDirectory(path.parent_path());
}

void CleanupStagingFiles(const fs::path& directory) {
    const auto status = ReadStat(directory);
    if (!status) {
        return;
    }
    ValidateOwnedDirectory(directory, "CCM metadata directory", false);
    for (const auto& entry : ListDirectory(directory)) {
        if (IsMetadataStagingName(entry.filename().string())) {
            ValidateStagingFile(entry);
            RemoveRegularFile(entry, "CCM metadata staging file");
        }
    }
}

std::vector<ManifestFile> ReadManifests(const fs::path& run_directory) {
    const fs::path owned = run_directory / "owned";
    if (!ReadStat(owned)) {
        return {};
    }
    ValidateOwnedDirectory(owned, "CCM manifest directory", true);
    std::vector<ManifestFile> manifests;
    for (const auto& entry : ListDirectory(owned)) {
        const auto name = entry.filename().string();
        if (entry.extension() != ".properties") {
            throw std::runtime_error("unexpected CCM ownership metadata at " + entry.string());
        }
        const auto values = ParseProperties(entry, {"format", "instance_id", "ccm_id"});
        const auto instance_id = values.at("instance_id");
        ValidateInstanceId(instance_id);
        if (name != instance_id + ".properties") {
            throw std::runtime_error("CCM manifest name does not match its instance at " + entry.string());
        }
        manifests.push_back({entry, instance_id, ParseCcmId(values.at("ccm_id"), entry)});
    }
    return manifests;
}

void ValidateOwnedTree(const fs::path& root) {
    ValidateOwnedDirectory(root, "CCM-owned directory", false);
    for (const auto& entry : ListDirectory(root)) {
        const auto status = ReadStat(entry);
        if (!status) {
            throw std::runtime_error("CCM-owned entry disappeared at " + entry.string());
        }
        RequireCurrentOwner(*status, entry, "CCM-owned entry");
        if (S_ISDIR(status->st_mode)) {
            ValidateOwnedTree(entry);
        } else if (!S_ISREG(status->st_mode)) {
            throw std::runtime_error("unsafe entry in CCM-owned tree at " + entry.string());
        }
    }
}

void DeleteValidatedTree(const fs::path& root) {
    ValidateOwnedDirectory(root, "CCM-owned directory", false);
    for (const auto& entry : ListDirectory(root)) {
        const auto status = ReadStat(entry);
        if (!status) {
            continue;
        }
        RequireCurrentOwner(*status, entry, "CCM-owned entry");
        if (S_ISDIR(status->st_mode)) {
            DeleteValidatedTree(entry);
        } else if (S_ISREG(status->st_mode)) {
            if (::unlink(entry.c_str()) != 0 && errno != ENOENT) {
                ThrowErrno("cannot remove CCM-owned file " + entry.string());
            }
        } else {
            throw std::runtime_error("refusing unsafe entry in CCM-owned tree at " + entry.string());
        }
    }
    if (::rmdir(root.c_str()) != 0 && errno != ENOENT) {
        ThrowErrno("cannot remove CCM-owned directory " + root.string());
    }
}

void RetireClusterConfig(const fs::path& run_directory, const std::string& instance_id) {
    const fs::path clusters = run_directory / "clusters";
    if (!ReadStat(clusters)) {
        return;
    }
    ValidateOwnedDirectory(clusters, "CCM clusters directory", false);
    const fs::path config = clusters / instance_id;
    if (!ReadStat(config)) {
        return;
    }
    ValidateOwnedDirectory(config, "CCM cluster config directory", false);
    for (const auto& entry : ListDirectory(config)) {
        const auto name = entry.filename().string();
        const auto status = ReadStat(entry);
        if (!status) {
            continue;
        }
        if (name == "tls" && S_ISDIR(status->st_mode)) {
            ValidateOwnedTree(entry);
        } else if (S_ISREG(status->st_mode) &&
                   (name == "ccm-commands.log" ||
                    (name.size() > 16 && name.compare(0, 12, "ccm-command-") == 0 &&
                     name.compare(name.size() - 4, 4, ".log") == 0))) {
            RequireCurrentOwner(*status, entry, "CCM command log");
        } else {
            throw std::runtime_error(
                "unexpected state remains in retired CCM config at " + entry.string());
        }
    }
    DeleteValidatedTree(config);
    FsyncDirectory(clusters);
}

void AssertNoUnownedClusterState(const fs::path& run_directory) {
    const fs::path clusters = run_directory / "clusters";
    if (!ReadStat(clusters)) {
        return;
    }
    ValidateOwnedDirectory(clusters, "CCM clusters directory", false);
    if (!ListDirectory(clusters).empty()) {
        throw std::runtime_error(
            "CCM run contains cluster state without a valid manifest: " + run_directory.string());
    }
}

fs::path IdLockPath(const fs::path& locks_directory, int ccm_id) {
    return locks_directory / (std::to_string(ccm_id) + ".lock");
}

fs::path IdOwnerPath(const fs::path& locks_directory, int ccm_id) {
    return locks_directory / (std::to_string(ccm_id) + ".owner");
}

std::optional<std::string> ReadReservationOwner(const fs::path& owner_path) {
    if (!ReadStat(owner_path)) {
        return std::nullopt;
    }
    const auto value = ReadMetadataFile(owner_path, "CCM ID reservation");
    if (value.empty() || value.back() != '\n' ||
        value.find('\n') != value.size() - 1 || !IsRunName(value.substr(0, value.size() - 1))) {
        throw std::runtime_error("malformed CCM ID reservation at " + owner_path.string());
    }
    return value.substr(0, value.size() - 1);
}

void DeleteReservationStagingFiles(const fs::path& locks_directory, int ccm_id) {
    const std::string prefix = ".owner-" + std::to_string(ccm_id) + ".owner-";
    for (const auto& entry : ListDirectory(locks_directory)) {
        const auto name = entry.filename().string();
        if (name.size() > prefix.size() + 4 && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - 4, 4, ".tmp") == 0) {
            ValidateStagingFile(entry);
            RemoveRegularFile(entry, "CCM reservation staging file");
        }
    }
}

void DeleteLegacyReservationStagingFiles(
    const fs::path& locks_directory,
    const std::string& run_name) {
    for (const auto& entry : ListDirectory(locks_directory)) {
        const auto name = entry.filename().string();
        if (name.size() < 12 || name.compare(0, 7, ".owner-") != 0 ||
            name.compare(name.size() - 4, 4, ".tmp") != 0) {
            continue;
        }
        const auto middle = name.substr(7, name.size() - 11);
        if (middle.empty() || !std::all_of(middle.begin(), middle.end(), [](char character) {
                return IsAsciiDigit(character);
            })) {
            continue;
        }
        try {
            if (ReadMetadataFile(entry, "legacy CCM reservation staging file") == run_name + "\n") {
                RemoveRegularFile(entry, "legacy CCM reservation staging file");
            }
        } catch (const std::exception&) {
            // Unrelated or unsafe legacy staging state remains quarantined.
        }
    }
}

bool IsAddressRangeAvailableRaw(int ccm_id, bool http, bool https, bool jmx) {
    if (ccm_id < kMinimumCcmId || ccm_id > kMaximumCcmId) {
        return false;
    }
    std::set<int> ports {
        kStoragePort,
        kCqlPort,
        kPrometheusPort,
        kApiPort,
        kShardAwareCqlPort,
    };
    if (jmx) {
        ports.insert(kJmxPort);
    }
    if (http) {
        ports.insert(kAlternatorHttpPort);
    }
    if (https) {
        ports.insert(kAlternatorHttpsPort);
    }

    for (int node = 1; node <= ClusterSpec::kMaximumNodeCount; ++node) {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        const std::string host =
            "127.0." + std::to_string(ccm_id) + "." + std::to_string(node);
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            return false;
        }
        for (int port : ports) {
            UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
            if (socket.Get() < 0) {
                return false;
            }
            address.sin_port = htons(static_cast<std::uint16_t>(port));
            if (::bind(
                    socket.Get(),
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) != 0) {
                return false;
            }
        }
    }
    return true;
}

struct ProcessUids {
    uid_t real;
    uid_t effective;
};

ProcessUids ReadProcessUids(pid_t pid) {
    const fs::path status_path = fs::path("/proc") / std::to_string(pid) / "status";
    const auto contents = ReadRawFile(status_path, kMaximumProcFileBytes);
    std::optional<std::string> uid_line;
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
            if (uid_line) {
                throw std::runtime_error("duplicate Uid field in " + status_path.string());
            }
            uid_line = line.substr(4);
        }
    }
    if (!uid_line) {
        throw std::runtime_error("missing Uid field in " + status_path.string());
    }
    std::istringstream values(*uid_line);
    std::array<std::uint64_t, 4> parsed {};
    for (auto& value : parsed) {
        if (!(values >> value)) {
            throw std::runtime_error("malformed Uid field in " + status_path.string());
        }
    }
    std::string extra;
    if (values >> extra || parsed[0] > std::numeric_limits<uid_t>::max() ||
        parsed[1] > std::numeric_limits<uid_t>::max()) {
        throw std::runtime_error("malformed Uid field in " + status_path.string());
    }
    return {static_cast<uid_t>(parsed[0]), static_cast<uid_t>(parsed[1])};
}

bool SharesIdentity(const ProcessUids& process) {
    return process.real == ::getuid() || process.real == ::geteuid() ||
           process.effective == ::getuid() || process.effective == ::geteuid();
}

bool ContainsNullDelimitedEntry(const std::string& contents, const std::string& expected) {
    std::size_t offset = 0;
    while (offset <= contents.size()) {
        const auto end = contents.find('\0', offset);
        const auto length = (end == std::string::npos ? contents.size() : end) - offset;
        if (length == expected.size() && contents.compare(offset, length, expected) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            return false;
        }
        offset = end + 1;
    }
    return false;
}

bool CommandReferencesRun(const std::string& command_line, const std::string& run_path) {
    std::size_t offset = 0;
    while (offset <= command_line.size()) {
        const auto end = command_line.find('\0', offset);
        const auto length = (end == std::string::npos ? command_line.size() : end) - offset;
        const auto argument = command_line.substr(offset, length);
        if (argument == run_path ||
            (argument.size() > run_path.size() && argument.compare(0, run_path.size(), run_path) == 0 &&
             argument[run_path.size()] == '/')) {
            return true;
        }
        if (end == std::string::npos) {
            return false;
        }
        offset = end + 1;
    }
    return false;
}

struct MarkedProcess {
    pid_t pid;
    std::uint64_t start_ticks;
    UniqueFd pid_fd;
};

UniqueFd OpenPidFd(pid_t pid, int* error) {
#if defined(SYS_pidfd_open)
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (fd >= 0) {
        *error = 0;
        return UniqueFd(fd);
    }
    *error = errno;
#else
    (void)pid;
    *error = ENOSYS;
#endif
    return UniqueFd();
}

bool PidFdIsLive(int pid_fd) {
    pollfd descriptor{pid_fd, POLLIN, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        ThrowErrno("cannot inspect marked CCM process pidfd");
    }
    if (result == 0) {
        return true;
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
        throw std::runtime_error("marked CCM process pidfd became invalid");
    }
    if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
        return false;
    }
    throw std::runtime_error("cannot determine marked CCM process state from pidfd");
}

bool IsAccessDenied(const std::system_error& error) {
    return error.code().value() == EACCES || error.code().value() == EPERM;
}

std::vector<MarkedProcess> FindMarkedProcesses(const fs::path& run_directory) {
    std::vector<MarkedProcess> processes;
    const std::string run_path = run_directory.string();
    for (const auto& entry : ListDirectory("/proc")) {
        const auto name = entry.filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char character) {
                return IsAsciiDigit(character);
            })) {
            continue;
        }
        std::uint64_t numeric_pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), numeric_pid);
        if (parsed.ec != std::errc() || parsed.ptr != name.data() + name.size() || numeric_pid == 0 ||
            numeric_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
            continue;
        }
        const auto pid = static_cast<pid_t>(numeric_pid);
        try {
            int pidfd_error = 0;
            auto pid_fd = OpenPidFd(pid, &pidfd_error);
            if (pid_fd.Get() < 0 && pidfd_error == ESRCH) {
                continue;
            }
            if (pid_fd.Get() >= 0 && !PidFdIsLive(pid_fd.Get())) {
                continue;
            }
            if (!SharesIdentity(ReadProcessUids(pid))) {
                continue;
            }
            const auto initial = ReadProcessStat(pid);
            if (!IsProcessActive(initial)) {
                continue;
            }
            bool command_references_run = false;
            try {
                command_references_run = CommandReferencesRun(
                    ReadRawFile(entry / "cmdline", kMaximumProcFileBytes),
                    run_path);
            } catch (const std::system_error& error) {
                if (!IsAccessDenied(error)) {
                    throw;
                }
            }

            bool marked = false;
            try {
                marked = ContainsNullDelimitedEntry(
                    ReadRawFile(entry / "environ", kMaximumProcFileBytes),
                    "SCYLLA_CCM_RUN_DIR=" + run_path);
            } catch (const std::system_error& error) {
                if (!IsAccessDenied(error)) {
                    throw;
                }
                if (command_references_run) {
                    throw std::runtime_error(
                        "cannot prove ownership of process " + std::to_string(pid) +
                        " whose command references stale CCM run " + run_directory.string());
                }
                continue;
            }
            const auto verified = ReadProcessStat(pid);
            if (!marked || !IsProcessActive(verified) ||
                verified.start_ticks != initial.start_ticks) {
                continue;
            }
            if (pid == ::getpid()) {
                throw std::runtime_error(
                    "current process carries a supposedly stale CCM run marker");
            }
            if (pid_fd.Get() < 0) {
                throw std::system_error(
                    pidfd_error,
                    std::generic_category(),
                    "cannot open pidfd for live marked CCM process " + std::to_string(pid));
            }
            if (!PidFdIsLive(pid_fd.Get())) {
                continue;
            }
            processes.push_back({pid, initial.start_ticks, std::move(pid_fd)});
        } catch (const std::system_error& error) {
            if (!IsMissingError(error)) {
                throw;
            }
        }
    }
    return processes;
}

bool ProcessMatches(const MarkedProcess& process) {
    try {
        if (!PidFdIsLive(process.pid_fd.Get())) {
            return false;
        }
        const auto current = ReadProcessStat(process.pid);
        return IsProcessActive(current) && current.start_ticks == process.start_ticks &&
               PidFdIsLive(process.pid_fd.Get());
    } catch (const std::system_error& error) {
        if (IsMissingError(error)) {
            return false;
        }
        throw;
    }
}

void SignalMarkedProcess(const MarkedProcess& process, int signal) {
    if (!ProcessMatches(process)) {
        return;
    }
#if defined(SYS_pidfd_send_signal)
    if (::syscall(SYS_pidfd_send_signal, process.pid_fd.Get(), signal, nullptr, 0) != 0 &&
        errno != ESRCH) {
        ThrowErrno("cannot signal marked CCM process through pidfd " +
                   std::to_string(process.pid));
    }
#else
    (void)signal;
    throw std::runtime_error("pidfd_send_signal is unavailable for marked CCM process cleanup");
#endif
}

bool CapturedMarkedProcessesExited(const std::vector<MarkedProcess>& processes) {
    return std::none_of(processes.begin(), processes.end(), ProcessMatches);
}

bool WaitForMarkedProcesses(
    const std::vector<MarkedProcess>& processes,
    const fs::path& run_directory,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (CapturedMarkedProcessesExited(processes) &&
            FindMarkedProcesses(run_directory).empty()) {
            return true;
        }
        std::this_thread::sleep_for(kProcessPollPeriod);
    } while (std::chrono::steady_clock::now() < deadline);
    return CapturedMarkedProcessesExited(processes) &&
           FindMarkedProcesses(run_directory).empty();
}

void TerminateMarkedProcesses(const fs::path& run_directory) {
    auto processes = FindMarkedProcesses(run_directory);
    for (const auto& process : processes) {
        SignalMarkedProcess(process, SIGTERM);
    }
    if (WaitForMarkedProcesses(
            processes,
            run_directory,
            std::chrono::duration_cast<std::chrono::milliseconds>(kTerminateGrace))) {
        return;
    }
    auto newly_marked = FindMarkedProcesses(run_directory);
    for (auto& process : newly_marked) {
        const bool already_captured = std::any_of(
            processes.begin(), processes.end(), [&](const MarkedProcess& captured) {
                return captured.pid == process.pid &&
                       captured.start_ticks == process.start_ticks;
            });
        if (!already_captured) {
            processes.push_back(std::move(process));
        }
    }
    for (const auto& process : processes) {
        SignalMarkedProcess(process, SIGKILL);
    }
    if (!WaitForMarkedProcesses(
            processes,
            run_directory,
            std::chrono::duration_cast<std::chrono::milliseconds>(kKillGrace))) {
        throw std::runtime_error("processes carrying stale CCM run marker did not terminate");
    }
}

void EnsureStaleReservation(
    const fs::path& locks_directory,
    int ccm_id,
    const std::string& expected_owner) {
    const fs::path owner_path = IdOwnerPath(locks_directory, ccm_id);
    auto actual_owner = ReadReservationOwner(owner_path);
    if (actual_owner && *actual_owner == expected_owner) {
        return;
    }
    if (actual_owner) {
        throw std::runtime_error(
            "CCM ID " + std::to_string(ccm_id) + " is reserved by a different run");
    }
    const fs::path lock_path = IdLockPath(locks_directory, ccm_id);
    UniqueFd lock(OpenLockFile(lock_path));
    if (!TryLockExclusive(lock.Get(), lock_path)) {
        throw std::runtime_error("CCM ID " + std::to_string(ccm_id) + " is still locked");
    }
    actual_owner = ReadReservationOwner(owner_path);
    if (!actual_owner) {
        WriteExclusive(owner_path, expected_owner + "\n");
    } else if (*actual_owner != expected_owner) {
        throw std::runtime_error(
            "CCM ID " + std::to_string(ccm_id) + " changed owner during stale cleanup");
    }
}

void ReleaseStaleReservation(
    const fs::path& locks_directory,
    int ccm_id,
    const std::string& expected_owner) {
    const fs::path owner_path = IdOwnerPath(locks_directory, ccm_id);
    auto actual_owner = ReadReservationOwner(owner_path);
    if (!actual_owner) {
        return;
    }
    if (*actual_owner != expected_owner) {
        throw std::runtime_error(
            "CCM ID " + std::to_string(ccm_id) + " is reserved by a different run");
    }
    const fs::path lock_path = IdLockPath(locks_directory, ccm_id);
    UniqueFd lock(OpenLockFile(lock_path));
    if (!TryLockExclusive(lock.Get(), lock_path)) {
        throw std::runtime_error("CCM ID " + std::to_string(ccm_id) + " is still locked");
    }
    DeleteReservationStagingFiles(locks_directory, ccm_id);
    actual_owner = ReadReservationOwner(owner_path);
    if (actual_owner && *actual_owner != expected_owner) {
        throw std::runtime_error(
            "CCM ID " + std::to_string(ccm_id) + " changed owner during stale cleanup");
    }
    RemoveRegularFile(owner_path, "CCM ID reservation");
}

std::vector<int> ReservationsOwnedBy(
    const fs::path& locks_directory,
    const std::string& run_name) {
    std::vector<int> reservations;
    for (int id = kMinimumCcmId; id <= kMaximumCcmId; ++id) {
        const fs::path owner_path = IdOwnerPath(locks_directory, id);
        try {
            const auto owner = ReadReservationOwner(owner_path);
            if (owner && *owner == run_name) {
                reservations.push_back(id);
            }
        } catch (const std::exception& exception) {
            std::cerr << "Preserving malformed CCM ID reservation at " << owner_path << ": "
                      << exception.what() << '\n';
        }
    }
    return reservations;
}

void ReleaseReservationsForMissingRuns(
    const fs::path& runs_directory,
    const fs::path& locks_directory) {
    for (int id = kMinimumCcmId; id <= kMaximumCcmId; ++id) {
        const fs::path owner_path = IdOwnerPath(locks_directory, id);
        std::optional<std::string> owner;
        try {
            owner = ReadReservationOwner(owner_path);
        } catch (const std::exception& exception) {
            std::cerr << "Preserving malformed CCM ID reservation at " << owner_path << ": "
                      << exception.what() << '\n';
            continue;
        }
        if (!owner) {
            continue;
        }
        const fs::path run = runs_directory / *owner;
        try {
            if (ReadStat(run)) {
                continue;
            }
            if (!IsAddressRangeAvailableRaw(id, true, true, true)) {
                std::cerr << "Preserving CCM ID reservation with occupied addresses at "
                          << owner_path << '\n';
                continue;
            }
            ReleaseStaleReservation(locks_directory, id, *owner);
        } catch (const std::exception& exception) {
            std::cerr << "Preserving CCM ID reservation after release failure at " << owner_path
                      << ": " << exception.what() << '\n';
        }
    }
}

void RecoverStaleRun(
    const fs::path& run,
    const fs::path& runs_directory,
    const fs::path& locks_directory,
    const CcmRunState::StaleClusterCleanup& cleanup) {
    ValidateRunDirectory(run, runs_directory);
    const fs::path owner_path = run / "OWNER";
    if (ReadStat(owner_path) && IsOwnerActive(ReadOwner(owner_path))) {
        return;
    }

    TerminateMarkedProcesses(run);
    CleanupStagingFiles(run);
    CleanupStagingFiles(run / "owned");
    auto manifests = ReadManifests(run);
    if (manifests.size() > 1) {
        throw std::runtime_error("CCM run contains more than one physical cluster manifest");
    }
    for (const auto& manifest : manifests) {
        EnsureStaleReservation(
            locks_directory,
            manifest.ccm_id,
            run.filename().string());
        if (!cleanup) {
            throw std::runtime_error("no stale CCM cluster cleanup callback is configured");
        }
        cleanup(run, manifest.instance_id, manifest.ccm_id);
        RetireClusterConfig(run, manifest.instance_id);
        RemoveRegularFile(manifest.path, "CCM cluster manifest");
    }
    AssertNoUnownedClusterState(run);
    CleanupStagingFiles(run / "owned");
    if (!ReadManifests(run).empty()) {
        throw std::runtime_error("CCM manifests remain after stale cleanup at " + run.string());
    }

    const std::string run_name = run.filename().string();
    DeleteLegacyReservationStagingFiles(locks_directory, run_name);
    const auto reservations = ReservationsOwnedBy(locks_directory, run_name);
    DeleteValidatedTree(run);
    FsyncDirectory(runs_directory);
    for (int id : reservations) {
        ReleaseStaleReservation(locks_directory, id, run_name);
    }
}

void RecoverStaleRuns(
    const fs::path& runs_directory,
    const fs::path& locks_directory,
    const CcmRunState::StaleClusterCleanup& cleanup) {
    for (const auto& run : ListDirectory(runs_directory)) {
        if (!IsRunName(run.filename().string())) {
            continue;
        }
        try {
            RecoverStaleRun(run, runs_directory, locks_directory, cleanup);
        } catch (const std::exception& exception) {
            std::cerr << "Preserving stale CCM run after cleanup failure at " << run << ": "
                      << exception.what() << '\n';
        } catch (...) {
            std::cerr << "Preserving stale CCM run after unknown cleanup failure at " << run << '\n';
        }
    }
}

fs::path CreateRunDirectory(const fs::path& runs_directory) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        const fs::path run = runs_directory / RandomRunName();
        if (::mkdir(run.c_str(), S_IRWXU) != 0) {
            if (errno == EEXIST) {
                continue;
            }
            ThrowErrno("cannot create CCM run directory " + run.string());
        }
        try {
            MakeDirectoryPrivate(run, "CCM run directory");
            const fs::path owned = PrepareChildDirectory(run, "owned");
            (void)owned;
            WriteExclusive(
                run / "OWNER",
                "format=" + std::to_string(kMetadataFormat) + "\npid=" +
                    std::to_string(::getpid()) + "\nstart_ticks=" +
                    std::to_string(ReadProcessStartTicks(::getpid())) + "\nboot_id=" +
                    CurrentBootId() + "\n");
            FsyncDirectory(runs_directory);
            ValidateRunDirectory(run, runs_directory);
            return run;
        } catch (...) {
            const auto failure = CurrentExceptionMessage();
            try {
                DeleteValidatedTree(run);
                FsyncDirectory(runs_directory);
            } catch (const std::exception& cleanup_error) {
                throw std::runtime_error(
                    failure + "; additionally failed to retire partial run: " + cleanup_error.what());
            }
            throw std::runtime_error(failure);
        }
    }
    throw std::runtime_error("unable to allocate unique CCM run directory");
}

struct IdReservation {
    int ccm_id;
    fs::path owner_path;
    UniqueFd lock;
};

IdReservation ReserveId(
    const fs::path& locks_directory,
    const std::string& run_name,
    const ClusterSpec& spec,
    bool include_jmx_port) {
    std::string last_failure;
    for (int id = kMinimumCcmId; id <= kMaximumCcmId; ++id) {
        const fs::path lock_path = IdLockPath(locks_directory, id);
        try {
            UniqueFd lock(OpenLockFile(lock_path));
            if (!TryLockExclusive(lock.Get(), lock_path)) {
                continue;
            }
            const fs::path owner_path = IdOwnerPath(locks_directory, id);
            if (ReadStat(owner_path) ||
                !CcmRunState::IsAddressRangeAvailable(spec, id, include_jmx_port)) {
                continue;
            }
            DeleteReservationStagingFiles(locks_directory, id);
            WriteExclusive(owner_path, run_name + "\n");
            return {id, owner_path, std::move(lock)};
        } catch (const MetadataPublicationError& failure) {
            last_failure = failure.what();
            if (!failure.RollbackDurable()) {
                throw;
            }
        } catch (...) {
            last_failure = CurrentExceptionMessage();
        }
    }
    throw std::runtime_error(
        "no CCM cluster IDs are available" +
        (last_failure.empty() ? std::string() : ": " + last_failure));
}

void ReleaseHeldReservation(
    const fs::path& locks_directory,
    const std::string& expected_owner,
    int ccm_id,
    const fs::path& owner_path,
    int& lock_fd) {
    if (lock_fd < 0) {
        throw std::invalid_argument("CCM reservation lock is not held");
    }
    auto actual_owner = ReadReservationOwner(owner_path);
    if (actual_owner && *actual_owner != expected_owner) {
        throw std::runtime_error("CCM ID reservation changed owner at " + owner_path.string());
    }
    DeleteReservationStagingFiles(locks_directory, ccm_id);
    actual_owner = ReadReservationOwner(owner_path);
    if (actual_owner && *actual_owner != expected_owner) {
        throw std::runtime_error("CCM ID reservation changed owner at " + owner_path.string());
    }
    RemoveRegularFile(owner_path, "CCM ID reservation");
    if (::flock(lock_fd, LOCK_UN) != 0) {
        ThrowErrno("cannot unlock CCM ID reservation " + std::to_string(ccm_id));
    }
    const int fd = lock_fd;
    lock_fd = -1;
    if (::close(fd) != 0) {
        ThrowErrno("cannot close CCM ID reservation " + std::to_string(ccm_id));
    }
}

} // namespace

void SetCcmRunStateMetadataPublishFsyncFailureCountdownForTest(std::uint32_t countdown) {
    metadata_publish_fsync_failure_countdown.store(countdown, std::memory_order_relaxed);
}

void SetCcmRunStateMetadataRollbackFsyncFailureForTest(bool enabled) {
    metadata_rollback_fsync_failure.store(enabled, std::memory_order_relaxed);
}

void SetCcmRunStatePasswdLookupUnavailableForTest(bool unavailable) {
    passwd_lookup_unavailable.store(unavailable, std::memory_order_relaxed);
}

CcmRunState::ClusterHandle::ClusterHandle() = default;

CcmRunState::ClusterHandle::~ClusterHandle() {
    if (lock_fd >= 0) {
        (void)::close(lock_fd);
    }
}

CcmRunState::ClusterHandle::ClusterHandle(ClusterHandle&& other) noexcept
    : ccm_id(other.ccm_id)
    , instance_id(std::move(other.instance_id))
    , lock_fd(other.lock_fd)
    , reservation_path(std::move(other.reservation_path))
    , manifest_path(std::move(other.manifest_path))
    , completed(other.completed) {
    other.ccm_id = 0;
    other.lock_fd = -1;
    other.completed = true;
}

CcmRunState::ClusterHandle& CcmRunState::ClusterHandle::operator=(ClusterHandle&& other) noexcept {
    if (this != &other) {
        if (lock_fd >= 0) {
            (void)::close(lock_fd);
        }
        ccm_id = other.ccm_id;
        instance_id = std::move(other.instance_id);
        lock_fd = other.lock_fd;
        reservation_path = std::move(other.reservation_path);
        manifest_path = std::move(other.manifest_path);
        completed = other.completed;
        other.ccm_id = 0;
        other.lock_fd = -1;
        other.completed = true;
    }
    return *this;
}

CcmRunState::CcmRunState(
    fs::path root,
    fs::path run_directory,
    StaleClusterCleanup cleanup)
    : root_(std::move(root))
    , runs_directory_(root_ / "runs")
    , locks_directory_(root_ / "ccm-id-locks")
    , run_directory_(std::move(run_directory))
    , cleanup_(std::move(cleanup)) {
}

std::unique_ptr<CcmRunState> CcmRunState::OpenDefault(StaleClusterCleanup cleanup) {
    const char* configured = std::getenv("SCYLLA_CCM_ROOT");
    fs::path root;
    if (configured != nullptr && !TrimAsciiWhitespace(configured).empty()) {
        root = fs::path(configured);
    } else {
        root = fs::path("/tmp") /
               ("alternator-client-cpp-ccm-" + std::to_string(::geteuid()));
    }
    return Open(root, std::move(cleanup));
}

std::unique_ptr<CcmRunState> CcmRunState::Open(
    const fs::path& requested_root,
    StaleClusterCleanup cleanup) {
    const fs::path root = PrepareRoot(requested_root);
    const fs::path runs = PrepareChildDirectory(root, "runs");
    const fs::path locks = PrepareChildDirectory(root, "ccm-id-locks");
    const fs::path scan_lock = root / "scan.lock";
    fs::path run;
    WithScanLock(scan_lock, [&] {
        RecoverStaleRuns(runs, locks, cleanup);
        ReleaseReservationsForMissingRuns(runs, locks);
        run = CreateRunDirectory(runs);
    });
    return std::unique_ptr<CcmRunState>(
        new CcmRunState(root, run, std::move(cleanup)));
}

CcmRunState::~CcmRunState() {
    try {
        Close();
    } catch (const std::exception& exception) {
        std::cerr << "Failed to retire CCM run state at " << run_directory_ << ": "
                  << exception.what() << '\n';
    } catch (...) {
        std::cerr << "Failed to retire CCM run state at " << run_directory_
                  << ": unknown failure\n";
    }
    if (incomplete_reservation_fd_ >= 0) {
        (void)::close(incomplete_reservation_fd_);
        incomplete_reservation_fd_ = -1;
    }
}

fs::path CcmRunState::RunDirectory() const {
    return run_directory_;
}

CcmRunState::ClusterHandle CcmRunState::BeginCluster(
    const std::string& instance_id,
    const ClusterSpec& spec,
    bool include_jmx_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::logic_error("CCM run state is closed");
    }
    if (incomplete_ownership_) {
        throw std::runtime_error("CCM run has incomplete ownership publication");
    }
    ValidateInstanceId(instance_id);
    spec.Validate();
    CleanupStagingFiles(run_directory_ / "owned");
    if (!ReadManifests(run_directory_).empty()) {
        throw std::logic_error("CCM run already owns a physical cluster");
    }
    AssertNoUnownedClusterState(run_directory_);

    IdReservation reservation;
    try {
        reservation = ReserveId(
            locks_directory_,
            run_directory_.filename().string(),
            spec,
            include_jmx_port);
    } catch (const MetadataPublicationError& failure) {
        if (!failure.RollbackDurable()) {
            incomplete_ownership_ = true;
        }
        throw;
    }
    const fs::path manifest = run_directory_ / "owned" / (instance_id + ".properties");
    try {
        WriteExclusive(
            manifest,
            "format=" + std::to_string(kMetadataFormat) + "\ninstance_id=" + instance_id +
                "\nccm_id=" + std::to_string(reservation.ccm_id) + "\n");
    } catch (const MetadataPublicationError& publication_failure) {
        if (!publication_failure.RollbackDurable()) {
            incomplete_ownership_ = true;
            incomplete_reservation_fd_ = reservation.lock.Release();
            throw;
        }
        const auto original_failure = std::string(publication_failure.what());
        int reservation_fd = reservation.lock.Release();
        try {
            ReleaseHeldReservation(
                locks_directory_,
                run_directory_.filename().string(),
                reservation.ccm_id,
                reservation.owner_path,
                reservation_fd);
        } catch (...) {
            incomplete_ownership_ = true;
            if (reservation_fd >= 0) {
                (void)::close(reservation_fd);
            }
            throw std::runtime_error(
                original_failure + "; additionally failed to release CCM reservation: " +
                CurrentExceptionMessage());
        }
        throw std::runtime_error(original_failure);
    } catch (...) {
        const auto original_failure = CurrentExceptionMessage();
        int reservation_fd = reservation.lock.Release();
        try {
            ReleaseHeldReservation(
                locks_directory_,
                run_directory_.filename().string(),
                reservation.ccm_id,
                reservation.owner_path,
                reservation_fd);
        } catch (...) {
            incomplete_ownership_ = true;
            if (reservation_fd >= 0) {
                (void)::close(reservation_fd);
            }
            throw std::runtime_error(
                original_failure + "; additionally failed to release CCM reservation: " +
                CurrentExceptionMessage());
        }
        throw std::runtime_error(original_failure);
    }

    ClusterHandle handle;
    handle.ccm_id = reservation.ccm_id;
    handle.instance_id = instance_id;
    handle.lock_fd = reservation.lock.Release();
    handle.reservation_path = std::move(reservation.owner_path);
    handle.manifest_path = manifest;
    handle.completed = false;
    return handle;
}

void CcmRunState::CompleteCluster(ClusterHandle& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::logic_error("CCM run state is closed");
    }
    if (handle.completed || handle.lock_fd < 0 || handle.ccm_id < kMinimumCcmId ||
        handle.ccm_id > kMaximumCcmId) {
        throw std::invalid_argument("CCM cluster handle is not active");
    }
    const fs::path expected_manifest =
        run_directory_ / "owned" / (handle.instance_id + ".properties");
    const fs::path expected_reservation = IdOwnerPath(locks_directory_, handle.ccm_id);
    if (handle.manifest_path != expected_manifest || handle.reservation_path != expected_reservation) {
        throw std::invalid_argument("CCM cluster handle is not owned by this run");
    }
    const auto values = ParseProperties(
        handle.manifest_path,
        {"format", "instance_id", "ccm_id"});
    if (values.at("instance_id") != handle.instance_id ||
        ParseCcmId(values.at("ccm_id"), handle.manifest_path) != handle.ccm_id) {
        throw std::invalid_argument("CCM cluster handle does not match its durable manifest");
    }
    const auto reservation_owner = ReadReservationOwner(handle.reservation_path);
    if (!reservation_owner || *reservation_owner != run_directory_.filename().string()) {
        throw std::runtime_error("CCM cluster reservation does not belong to this run");
    }

    incomplete_ownership_ = true;
    RetireClusterConfig(run_directory_, handle.instance_id);
    RemoveRegularFile(handle.manifest_path, "CCM cluster manifest");
    ReleaseHeldReservation(
        locks_directory_,
        run_directory_.filename().string(),
        handle.ccm_id,
        handle.reservation_path,
        handle.lock_fd);
    handle.completed = true;
    incomplete_ownership_ = false;
}

void CcmRunState::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return;
    }
    if (incomplete_ownership_) {
        throw std::runtime_error("cannot retire CCM run with incomplete ownership publication");
    }

    WithScanLock(root_ / "scan.lock", [&] {
        const auto status = ReadStat(run_directory_);
        if (!status) {
            return;
        }
        ValidateRunDirectory(run_directory_, runs_directory_);
        CleanupStagingFiles(run_directory_);
        CleanupStagingFiles(run_directory_ / "owned");
        if (!ReadManifests(run_directory_).empty()) {
            throw std::runtime_error("cannot retire CCM run while it still owns a cluster");
        }
        AssertNoUnownedClusterState(run_directory_);
        if (!ReservationsOwnedBy(
                locks_directory_,
                run_directory_.filename().string()).empty()) {
            throw std::runtime_error("cannot retire CCM run while it still owns an address range");
        }
        DeleteValidatedTree(run_directory_);
        FsyncDirectory(runs_directory_);
    });
    closed_ = true;
}

bool CcmRunState::IsAddressRangeAvailable(
    const ClusterSpec& spec,
    int ccm_id,
    bool include_jmx_port) {
    return IsAddressRangeAvailableRaw(
        ccm_id,
        spec.Transports().count(AlternatorTransport::Http) != 0,
        spec.Transports().count(AlternatorTransport::Https) != 0,
        include_jmx_port);
}

} // namespace scylladb::alternator::testinfra
