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

#include <scylladb/alternator/http_client.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <spawn.h>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace scylladb::alternator::testinfra {
namespace {

namespace fs = std::filesystem;

constexpr std::uint16_t kHttpPort = 8080;
constexpr std::uint16_t kHttpsPort = 8043;
constexpr auto kReadinessTimeout = std::chrono::minutes(5);
constexpr auto kHealthTimeout = std::chrono::seconds(10);
constexpr auto kStopTimeout = std::chrono::seconds(10);
constexpr auto kTermGrace = std::chrono::seconds(2);
constexpr auto kKillGrace = std::chrono::seconds(5);
constexpr std::string_view kPinnedCcmCommit =
    "d15a2fab9d22fffad8a30c806a7c8e1632e58aae";
constexpr std::string_view kTestUser = "alternator_tests";
constexpr std::string_view kTestSaltedPassword =
    "$6$IcPWfCigHWVhHTf.$h3."
    "30m5R2CnYqIeniCumbXCBxBxvtYPP3MbZVsjKcu268ESOcrUtSJwf1iO1s83KUT3waITRtTiex"
    "BdSWEI0Q/";
constexpr std::string_view kGossipingPropertyFileSnitch =
    "org.apache.cassandra.locator.GossipingPropertyFileSnitch";

class CommandError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ProcessCleanupError final : public RecoveryRequiredError {
public:
  using RecoveryRequiredError::RecoveryRequiredError;
};

std::string ErrnoMessage(const std::string &operation) {
  return operation + ": " + std::strerror(errno);
}

std::string GetEnvironment(const char *name, std::string fallback = {}) {
  const char *value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::move(fallback)
                                            : std::string(value);
}

fs::path AbsoluteNormalized(const fs::path &path) {
  return fs::absolute(path).lexically_normal();
}

bool PathStartsWith(const fs::path &path, const fs::path &prefix) {
  auto current = path.begin();
  auto expected = prefix.begin();
  while (expected != prefix.end()) {
    if (current == path.end() || *current != *expected) {
      return false;
    }
    ++current;
    ++expected;
  }
  return true;
}

void SetOwnerOnlyDirectoryPermissions(const fs::path &directory) {
  fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
}

void ValidateOwnedDirectory(const fs::path &directory,
                            const std::string &description) {
  const auto status = fs::symlink_status(directory);
  if (fs::is_symlink(status) || !fs::is_directory(status)) {
    throw std::runtime_error("Refusing unsafe " + description + " directory " +
                             directory.string());
  }

  struct stat metadata {};
  if (::lstat(directory.c_str(), &metadata) != 0) {
    throw std::runtime_error(ErrnoMessage("lstat " + directory.string()));
  }
  if (metadata.st_uid != ::geteuid()) {
    throw std::runtime_error(
        description +
        " directory is not owned by current user: " + directory.string());
  }
  if (fs::canonical(directory) != AbsoluteNormalized(directory)) {
    throw std::runtime_error(
        description +
        " directory traverses a symbolic link: " + directory.string());
  }
}

void RejectSymbolicLinkComponents(const fs::path &path,
                                  const std::string &description) {
  for (auto current = AbsoluteNormalized(path); !current.empty();
       current = current.parent_path()) {
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::system_error(error,
                              "Unable to inspect " + description + " path " +
                                  current.string());
    }
    if (!error && fs::is_symlink(status)) {
      throw std::runtime_error("Refusing " + description +
                               " path containing symbolic link " +
                               current.string());
    }
    if (current == current.root_path()) {
      break;
    }
  }
}

void PrepareDirectory(const fs::path &directory,
                      const std::string &description) {
  RejectSymbolicLinkComponents(directory, description);
  fs::create_directories(directory);
  RejectSymbolicLinkComponents(directory, description);
  ValidateOwnedDirectory(directory, description);
  SetOwnerOnlyDirectoryPermissions(directory);
}

void PrepareDirectChildDirectory(const fs::path &parent, const fs::path &child,
                                 const std::string &description) {
  const auto normalized_parent = AbsoluteNormalized(parent);
  const auto normalized_child = AbsoluteNormalized(child);
  if (normalized_child.parent_path() != normalized_parent) {
    throw std::runtime_error("Refusing " + description + " outside " +
                             normalized_parent.string());
  }
  RejectSymbolicLinkComponents(normalized_child, description);
  fs::create_directory(normalized_child);
  RejectSymbolicLinkComponents(normalized_child, description);
  ValidateOwnedDirectory(normalized_child, description);
  SetOwnerOnlyDirectoryPermissions(normalized_child);
}

fs::path ConfiguredDiagnosticsDirectory() {
  const auto configured = GetEnvironment("SCYLLA_CCM_DIAGNOSTICS_DIR");
  if (!configured.empty()) {
    return AbsoluteNormalized(configured);
  }
  return AbsoluteNormalized(fs::current_path() / "build" / "ccm");
}

fs::path ValidateOperationalDiagnostics(const fs::path &run_directory,
                                        const fs::path &diagnostics_directory) {
  const auto run = AbsoluteNormalized(run_directory);
  const auto runs = run.parent_path();
  const auto root = runs.parent_path();
  const auto diagnostics = AbsoluteNormalized(diagnostics_directory);
  if (runs.filename() != "runs" || root.empty()) {
    throw std::runtime_error(
        "Operational CCM run directory is not beneath a runs directory");
  }
  if (PathStartsWith(diagnostics, root) || PathStartsWith(root, diagnostics)) {
    throw std::runtime_error(
        "CCM diagnostics directory must not overlap harness state root " +
        root.string());
  }
  return diagnostics;
}

fs::path ValidateDiagnosticsSeparation(const fs::path &run_directory,
                                       const fs::path &diagnostics_directory) {
  const auto run = AbsoluteNormalized(run_directory);
  const auto diagnostics = AbsoluteNormalized(diagnostics_directory);
  const auto state_root = run.parent_path().filename() == "runs"
                              ? run.parent_path().parent_path()
                              : run;
  if (PathStartsWith(diagnostics, state_root) ||
      PathStartsWith(state_root, diagnostics)) {
    throw std::runtime_error(
        "CCM diagnostics directory must not overlap harness state at " +
        state_root.string());
  }
  return diagnostics;
}

void ValidateInstanceId(const std::string &instance_id) {
  static const std::regex pattern("[A-Za-z0-9][A-Za-z0-9-]{0,127}");
  if (!std::regex_match(instance_id, pattern)) {
    throw std::invalid_argument("Unsafe CCM cluster instance ID: " +
                                instance_id);
  }
}

void ValidateCcmId(int ccm_id) {
  if (ccm_id < 1 || ccm_id > 99) {
    throw std::invalid_argument("CCM ID must be between 1 and 99");
  }
}

void ValidateArgument(const std::string &value,
                      const std::string &description) {
  if (value.empty() || value.find('\0') != std::string::npos) {
    throw std::invalid_argument(description +
                                " must be non-empty and contain no NUL byte");
  }
}

std::string ReadFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Unable to read " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("Unable to read " + path.string());
  }
  return contents.str();
}

void WriteAll(int fd, std::string_view contents) {
  while (!contents.empty()) {
    const auto written = ::write(fd, contents.data(), contents.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(ErrnoMessage("write"));
    }
    contents.remove_prefix(static_cast<std::size_t>(written));
  }
}

std::string QuoteForLog(const std::string &argument) {
  if (argument.find_first_of(" \t\r\n'\\") == std::string::npos) {
    return argument;
  }
  std::string quoted = "'";
  for (const char character : argument) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

std::string FormatCommand(const std::vector<std::string> &command) {
  std::ostringstream output;
  bool first = true;
  for (const auto &argument : command) {
    if (!first) {
      output << ' ';
    }
    first = false;
    output << QuoteForLog(argument);
  }
  return output.str();
}

bool IsEnvironmentVariable(const std::string &entry, const char *name) {
  const std::string_view variable(name);
  return entry.size() > variable.size() &&
         entry.compare(0, variable.size(), variable) == 0 &&
         entry[variable.size()] == '=';
}

std::string ScyllaArchitectureForHost() {
  struct utsname host {};
  if (::uname(&host) != 0) {
    throw std::runtime_error(ErrnoMessage("uname"));
  }
  const std::string machine(host.machine);
  if (machine == "x86_64" || machine == "amd64") {
    return "x86_64";
  }
  if (machine == "aarch64" || machine == "arm64") {
    return "aarch64";
  }
  throw std::runtime_error("Unsupported Linux architecture for CCM: " +
                           machine);
}

std::vector<std::string> ChildEnvironment(const fs::path &run_directory) {
  std::vector<std::string> environment;
  for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    std::string value(*entry);
    if (IsEnvironmentVariable(value, "SCYLLA_EXT_ENV") ||
        IsEnvironmentVariable(value, "SCYLLA_EXT_OPTS") ||
        IsEnvironmentVariable(value, "SCYLLA_MANAGER_PACKAGE") ||
        IsEnvironmentVariable(value, "SCYLLA_CCM_RUN_DIR") ||
        IsEnvironmentVariable(value, "SCYLLA_ARCH")) {
      continue;
    }
    environment.push_back(std::move(value));
  }
  environment.push_back("SCYLLA_CCM_RUN_DIR=" + run_directory.string());
  environment.push_back("SCYLLA_ARCH=" + ScyllaArchitectureForHost());
  return environment;
}

std::vector<char *> MutablePointers(std::vector<std::string> *values) {
  std::vector<char *> pointers;
  pointers.reserve(values->size() + 1);
  for (auto &value : *values) {
    pointers.push_back(value.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

fs::path CreateCommandLog(const fs::path &ccm_directory,
                          std::uint64_t first_sequence,
                          const std::string &command_text, int *output_fd) {
  for (std::uint64_t offset = 0; offset < 1000; ++offset) {
    std::ostringstream name;
    name << "ccm-command-" << ::getpid() << '-' << std::setw(8)
         << std::setfill('0') << (first_sequence + offset) << ".log";
    const auto path = ccm_directory / name.str();
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) {
        continue;
      }
      throw std::runtime_error(ErrnoMessage("open " + path.string()));
    }
    try {
      WriteAll(fd, "> " + command_text + "\n");
      if (::fsync(fd) != 0) {
        throw std::runtime_error(ErrnoMessage("fsync " + path.string()));
      }
    } catch (...) {
      ::close(fd);
      throw;
    }
    *output_fd = fd;
    return path;
  }
  throw std::runtime_error("Unable to allocate a unique CCM command log");
}

void AppendCommandOutcome(const fs::path &output_path,
                          const std::string &outcome) {
  const int fd = ::open(output_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error(ErrnoMessage("open " + output_path.string()));
  }
  try {
    WriteAll(fd, "\n[exit " + outcome + "]\n");
    if (::fsync(fd) != 0) {
      throw std::runtime_error(ErrnoMessage("fsync " + output_path.string()));
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) {
    throw std::runtime_error(ErrnoMessage("close " + output_path.string()));
  }
}

void AppendAggregateLog(const fs::path &ccm_directory,
                        const fs::path &output_path) {
  const auto contents = ReadFile(output_path);
  const auto aggregate_path = ccm_directory / "ccm-commands.log";
  const int fd = ::open(aggregate_path.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error(ErrnoMessage("open " + aggregate_path.string()));
  }
  try {
    WriteAll(fd, contents);
    if (contents.empty() || contents.back() != '\n') {
      WriteAll(fd, "\n");
    }
    if (::fsync(fd) != 0) {
      throw std::runtime_error(
          ErrnoMessage("fsync " + aggregate_path.string()));
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) {
    throw std::runtime_error(ErrnoMessage("close " + aggregate_path.string()));
  }
}

bool ReapChildNonBlocking(pid_t pid, int *status, bool *reaped) {
  if (*reaped) {
    return true;
  }
  while (true) {
    const pid_t result = ::waitpid(pid, status, WNOHANG);
    if (result == pid) {
      *reaped = true;
      return true;
    }
    if (result == 0) {
      return false;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == ECHILD) {
      *status = -1;
      *reaped = true;
      return true;
    }
    throw std::runtime_error(ErrnoMessage("waitpid"));
  }
}

bool ProcessAlive(pid_t pid);

bool ProcessGroupAlive(pid_t process_group) {
  if (::kill(-process_group, 0) != 0) {
    return errno == EPERM;
  }

  std::error_code scan_error;
  fs::directory_iterator process(fs::path("/proc"), scan_error);
  const fs::directory_iterator end;
  while (!scan_error && process != end) {
    const auto name = process->path().filename().string();
    pid_t pid = 0;
    const auto parsed =
        std::from_chars(name.data(), name.data() + name.size(), pid);
    if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() &&
        pid > 0) {
      errno = 0;
      const pid_t observed_group = ::getpgid(pid);
      if (observed_group < 0 && errno != ESRCH) {
        return true;
      }
      if (observed_group == process_group && ProcessAlive(pid)) {
        errno = 0;
        const pid_t verified_group = ::getpgid(pid);
        if (verified_group == process_group ||
            (verified_group < 0 && errno != ESRCH)) {
          return true;
        }
      }
    }
    process.increment(scan_error);
  }
  // Failure to inspect the complete process table cannot prove termination.
  return static_cast<bool>(scan_error);
}

void SignalProcessGroup(pid_t process_group, int signal) {
  if (::kill(-process_group, signal) != 0 && errno != ESRCH) {
    throw ProcessCleanupError(ErrnoMessage("kill process group"));
  }
}

void SignalDirectProcess(pid_t pid, int signal) {
  if (::kill(pid, signal) != 0 && errno != ESRCH) {
    throw ProcessCleanupError(ErrnoMessage("kill process"));
  }
}

bool WaitForProcessGroup(pid_t pid,
                         std::chrono::steady_clock::time_point deadline,
                         int *status, bool *reaped) {
  while (std::chrono::steady_clock::now() < deadline) {
    ReapChildNonBlocking(pid, status, reaped);
    if (!ProcessGroupAlive(pid) && *reaped) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ReapChildNonBlocking(pid, status, reaped);
  return !ProcessGroupAlive(pid) && *reaped;
}

void TerminateProcessGroup(pid_t pid, int *status, bool *reaped) {
  SignalProcessGroup(pid, SIGTERM);
  if (!*reaped) {
    SignalDirectProcess(pid, SIGTERM);
  }
  if (WaitForProcessGroup(pid, std::chrono::steady_clock::now() + kTermGrace,
                          status, reaped)) {
    return;
  }

  SignalProcessGroup(pid, SIGKILL);
  if (!*reaped) {
    SignalDirectProcess(pid, SIGKILL);
  }
  if (!WaitForProcessGroup(pid, std::chrono::steady_clock::now() + kKillGrace,
                           status, reaped)) {
    throw ProcessCleanupError(
        "Unable to prove cleanup of CCM command process group " +
        std::to_string(pid));
  }
}

bool IsSuccessfulExit(int status) {
  return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string ExitDescription(int status, bool timed_out) {
  if (timed_out) {
    return "timeout";
  }
  if (status < 0) {
    return "unknown (child status was reaped externally)";
  }
  if (WIFEXITED(status)) {
    return std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "signal " + std::to_string(WTERMSIG(status));
  }
  return "unknown";
}

std::string Authenticator(AuthenticationMode mode) {
  switch (mode) {
  case AuthenticationMode::Password:
    return "org.apache.cassandra.auth.PasswordAuthenticator";
  case AuthenticationMode::Transitional:
    return "com.scylladb.auth.TransitionalAuthenticator";
  case AuthenticationMode::AllowAll:
    break;
  }
  throw std::invalid_argument("No authenticator for allow-all authentication");
}

std::string Authorizer(AuthorizationMode mode) {
  switch (mode) {
  case AuthorizationMode::Cassandra:
    return "org.apache.cassandra.auth.CassandraAuthorizer";
  case AuthorizationMode::Transitional:
    return "com.scylladb.auth.TransitionalAuthorizer";
  case AuthorizationMode::AllowAll:
    break;
  }
  throw std::invalid_argument("No authorizer for allow-all authorization");
}

bool HasTransport(const ClusterSpec &spec, AlternatorTransport transport) {
  return spec.Transports().find(transport) != spec.Transports().end();
}

std::vector<TestClusterNode> BuildNodes(const ClusterTopology &topology,
                                        int ccm_id) {
  std::vector<TestClusterNode> nodes;
  int node_index = 0;

  for (std::size_t dc_index = 0; dc_index < topology.Datacenters().size();
       ++dc_index) {
    const auto count =
        topology.Datacenters()[dc_index].Racks().front().NodeCount();
    for (int offset = 0; offset < count; ++offset) {
      ++node_index;
      nodes.push_back(TestClusterNode{
          "node" + std::to_string(node_index),
          "127.0." + std::to_string(ccm_id) + "." + std::to_string(node_index),
          "dc" + std::to_string(dc_index + 1),
          "RAC1",
      });
    }
  }

  for (std::size_t dc_index = 0; dc_index < topology.Datacenters().size();
       ++dc_index) {
    const auto &datacenter = topology.Datacenters()[dc_index];
    for (std::size_t rack_index = 1; rack_index < datacenter.Racks().size();
         ++rack_index) {
      for (int offset = 0; offset < datacenter.Racks()[rack_index].NodeCount();
           ++offset) {
        ++node_index;
        nodes.push_back(TestClusterNode{
            "node" + std::to_string(node_index),
            "127.0." + std::to_string(ccm_id) + "." +
                std::to_string(node_index),
            "dc" + std::to_string(dc_index + 1),
            "RAC" + std::to_string(rack_index + 1),
        });
      }
    }
  }
  return nodes;
}

std::string FirstRackCounts(const ClusterTopology &topology) {
  std::ostringstream counts;
  for (std::size_t index = 0; index < topology.Datacenters().size(); ++index) {
    if (index != 0) {
      counts << ':';
    }
    counts << topology.Datacenters()[index].Racks().front().NodeCount();
  }
  return counts.str();
}

std::vector<std::string> StartArguments(const ClusterSpec &spec,
                                        const fs::path &ccm_directory,
                                        const std::string &node_name) {
  return {
      node_name,
      "start",
      "--config-dir",
      ccm_directory.string(),
      "--wait-for-binary-proto",
      "--wait-other-notice",
      "--jvm_arg=--smp",
      "--jvm_arg=" + std::to_string(spec.Resources().Smp()),
      "--jvm_arg=--memory",
      "--jvm_arg=" + std::to_string(spec.Resources().MemoryMiB()) + "M",
  };
}

std::shared_ptr<HttpClient>
ReadinessClient(AlternatorTransport transport,
                const fs::path &ca_certificate_path) {
  Config config;
  config.scheme = transport == AlternatorTransport::Http ? "http" : "https";
  config.port = transport == AlternatorTransport::Http ? kHttpPort : kHttpsPort;
  config.connect_timeout = std::chrono::seconds(5);
  config.http_client_timeout = std::chrono::seconds(5);
  config.reuse_discovery_connections = false;
  if (transport == AlternatorTransport::Https) {
    if (ca_certificate_path.empty()) {
      throw std::runtime_error(
          "HTTPS readiness requires a generated CA certificate");
    }
    config.verify_ssl = true;
    config.ca_file = ca_certificate_path.string();
  }
  return NewDefaultHttpClient(config);
}

void WaitForEndpoints(const ClusterSpec &spec,
                      const std::vector<TestClusterNode> &nodes,
                      const fs::path &ca_certificate_path,
                      std::chrono::steady_clock::duration timeout) {
  std::shared_ptr<HttpClient> http;
  std::shared_ptr<HttpClient> https;
  if (HasTransport(spec, AlternatorTransport::Http)) {
    http = ReadinessClient(AlternatorTransport::Http, {});
  }
  if (HasTransport(spec, AlternatorTransport::Https)) {
    https = ReadinessClient(AlternatorTransport::Https, ca_certificate_path);
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string last_error;
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_ready = true;
    for (const auto &node : nodes) {
      try {
        if (http) {
          const auto response =
              http->Get(Url::FromHostPort("http", node.address, kHttpPort)
                            .WithPathAndQuery("/"));
          if (response.status_code < 200 || response.status_code >= 300) {
            throw std::runtime_error("HTTP status " +
                                     std::to_string(response.status_code));
          }
        }
        if (https) {
          const auto response =
              https->Get(Url::FromHostPort("https", node.address, kHttpsPort)
                             .WithPathAndQuery("/"));
          if (response.status_code < 200 || response.status_code >= 300) {
            throw std::runtime_error("HTTPS status " +
                                     std::to_string(response.status_code));
          }
        }
      } catch (const std::exception &exception) {
        last_error = exception.what();
        all_ready = false;
        break;
      }
    }
    if (all_ready) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  throw std::runtime_error(
      "Alternator endpoints did not become ready" +
      (last_error.empty() ? std::string() : ": " + last_error));
}

bool IsNodeName(const std::string &name) {
  static const std::regex pattern("node[1-9][0-9]*");
  return std::regex_match(name, pattern);
}

std::optional<pid_t> ParsePid(const std::string &text) {
  auto value = text;
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::nullopt;
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  value = value.substr(first, last - first + 1);
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= '0' && character <= '9';
      })) {
    return std::nullopt;
  }
  try {
    std::size_t parsed_characters = 0;
    const long long parsed = std::stoll(value, &parsed_characters);
    if (parsed < 2 || parsed > std::numeric_limits<pid_t>::max()) {
      return std::nullopt;
    }
    if (parsed_characters != value.size()) {
      return std::nullopt;
    }
    return static_cast<pid_t>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<pid_t> ReadPidFile(const fs::path &path) {
  if (!fs::exists(path)) {
    return std::nullopt;
  }
  if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path)) {
    throw ProcessCleanupError("Refusing unsafe PID file " + path.string());
  }
  const auto pid = ParsePid(ReadFile(path));
  if (!pid) {
    throw ProcessCleanupError("Invalid PID reference in " + path.string());
  }
  return pid;
}

std::optional<pid_t> ReadNodeConfigPid(const fs::path &node_config) {
  if (!fs::exists(node_config)) {
    return std::nullopt;
  }
  if (fs::is_symlink(fs::symlink_status(node_config)) ||
      !fs::is_regular_file(node_config)) {
    throw ProcessCleanupError("Refusing unsafe node configuration " +
                              node_config.string());
  }
  std::istringstream input(ReadFile(node_config));
  std::string line;
  std::optional<pid_t> result;
  while (std::getline(input, line)) {
    if (line.rfind("pid:", 0) == 0) {
      if (result) {
        throw ProcessCleanupError("Duplicate PID reference in " +
                                  node_config.string());
      }
      auto value = line.substr(4);
      const auto first = value.find_first_not_of(" \t\r\n");
      const auto last = value.find_last_not_of(" \t\r\n");
      value = first == std::string::npos
                  ? std::string()
                  : value.substr(first, last - first + 1);
      if (value.size() >= 2 &&
          ((value.front() == '\'' && value.back() == '\'') ||
           (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
      }
      const auto pid = ParsePid(value);
      if (!pid) {
        throw ProcessCleanupError("Invalid PID reference in " +
                                  node_config.string());
      }
      result = pid;
    }
  }
  return result;
}

std::set<pid_t> ReadNodePids(const fs::path &node_directory) {
  std::set<pid_t> pids;
  if (const auto pid = ReadPidFile(node_directory / "cassandra.pid")) {
    pids.insert(*pid);
  }
  if (const auto pid = ReadNodeConfigPid(node_directory / "node.conf")) {
    pids.insert(*pid);
  }
  return pids;
}

bool ProcessAlive(pid_t pid) {
  if (::kill(pid, 0) != 0 && errno == ESRCH) {
    return false;
  }
  const auto stat_path = fs::path("/proc") / std::to_string(pid) / "stat";
  try {
    const auto stat = ReadFile(stat_path);
    const auto command_end = stat.rfind(')');
    if (command_end != std::string::npos && command_end + 2 < stat.size()) {
      const char state = stat[command_end + 2];
      return state != 'Z' && state != 'X' && state != 'x';
    }
  } catch (...) {
    // A PID known to exist remains live until its disappearance is proven.
    return true;
  }
  return true;
}

std::uint64_t ReadProcessStartTicks(pid_t pid) {
  const auto stat =
      ReadFile(fs::path("/proc") / std::to_string(pid) / "stat");
  const auto command_end = stat.rfind(')');
  if (command_end == std::string::npos || command_end + 2 >= stat.size()) {
    throw ProcessCleanupError("Unable to parse process identity for PID " +
                              std::to_string(pid));
  }
  std::istringstream fields(stat.substr(command_end + 2));
  std::string value;
  for (int index = 0; index <= 19; ++index) {
    if (!(fields >> value)) {
      throw ProcessCleanupError("Unable to parse process identity for PID " +
                                std::to_string(pid));
    }
  }
  try {
    std::size_t parsed_characters = 0;
    const auto ticks = std::stoull(value, &parsed_characters);
    if (ticks == 0 || parsed_characters != value.size()) {
      throw std::invalid_argument("invalid process start ticks");
    }
    return ticks;
  } catch (...) {
    throw ProcessCleanupError("Unable to parse process identity for PID " +
                              std::to_string(pid));
  }
}

bool ProcessOwnedByCurrentUser(pid_t pid) {
  struct stat metadata {};
  const auto process_directory =
      fs::path("/proc") / std::to_string(pid);
  if (::lstat(process_directory.c_str(), &metadata) != 0) {
    return false;
  }
  return metadata.st_uid == ::geteuid();
}

bool ContainsEnvironmentEntry(const std::string &environment,
                              const std::string &expected) {
  std::size_t start = 0;
  while (start <= environment.size()) {
    const auto end = environment.find('\0', start);
    const auto length =
        end == std::string::npos ? environment.size() - start : end - start;
    if (environment.compare(start, length, expected) == 0 &&
        length == expected.size()) {
      return true;
    }
    if (end == std::string::npos) {
      return false;
    }
    start = end + 1;
  }
  return false;
}

std::vector<std::string> ProcessArguments(pid_t pid) {
  const auto command_line =
      ReadFile(fs::path("/proc") / std::to_string(pid) / "cmdline");
  std::vector<std::string> arguments;
  std::size_t start = 0;
  while (start < command_line.size()) {
    const auto end = command_line.find('\0', start);
    arguments.push_back(command_line.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return arguments;
}

bool ProcessHasRunMarker(pid_t pid, const fs::path &run_directory) {
  const auto environment =
      ReadFile(fs::path("/proc") / std::to_string(pid) / "environ");
  return ContainsEnvironmentEntry(environment, "SCYLLA_CCM_RUN_DIR=" +
                                                   run_directory.string());
}

enum class ProcessReferenceKind {
  Scylla,
  Jmx,
  Agent,
};

bool ContainsArgumentPair(const std::vector<std::string> &arguments,
                          const std::string &option,
                          const std::string &value) {
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] == option && arguments[index + 1] == value) {
      return true;
    }
  }
  return false;
}

bool ProcessBelongsToNode(pid_t pid, const fs::path &node_directory,
                          const fs::path &run_directory,
                          ProcessReferenceKind kind) {
  try {
    if (!ProcessOwnedByCurrentUser(pid)) {
      return false;
    }
    const auto start_ticks = ReadProcessStartTicks(pid);
    if (!ProcessHasRunMarker(pid, run_directory)) {
      return false;
    }
    const auto arguments = ProcessArguments(pid);
    if (arguments.empty()) {
      return false;
    }
    const auto &executable = arguments.front();
    bool matches = false;
    switch (kind) {
    case ProcessReferenceKind::Scylla:
      matches = executable == (node_directory / "bin" / "scylla").string();
      break;
    case ProcessReferenceKind::Agent:
      matches =
          executable ==
              (node_directory / "bin" / "scylla-manager-agent").string() &&
          ContainsArgumentPair(
              arguments,
              "--config-file",
              (node_directory / "conf" / "scylla-manager-agent.yaml").string());
      break;
    case ProcessReferenceKind::Jmx: {
      const auto launcher =
          (node_directory / "bin" / "symlinks" / "scylla-jmx").string();
      const bool launcher_shape = executable == launcher;
      const bool java_shape = fs::path(executable).filename() == "java";
      if (java_shape) {
        const auto process_executable = fs::read_symlink(
            fs::path("/proc") / std::to_string(pid) / "exe");
        if (process_executable.filename() != "java") {
          return false;
        }
      }
      matches =
          (launcher_shape || java_shape) &&
          ContainsArgumentPair(
              arguments,
              "-jar",
              (node_directory / "bin" / "scylla-jmx-1.0.jar").string());
      break;
    }
    }
    return matches && ReadProcessStartTicks(pid) == start_ticks;
  } catch (...) {
    return false;
  }
  return false;
}

struct CapturedProcessReference {
  pid_t pid;
  std::uint64_t start_ticks;
  ProcessReferenceKind kind;
  fs::path node_directory;
};

CapturedProcessReference CaptureProcessReference(
    pid_t pid, const fs::path &node_directory,
    const fs::path &run_directory, ProcessReferenceKind kind) {
  const auto start_ticks = ReadProcessStartTicks(pid);
  if (!ProcessBelongsToNode(pid, node_directory, run_directory, kind) ||
      ReadProcessStartTicks(pid) != start_ticks) {
    throw ProcessCleanupError("CCM node references unrelated or changing live PID " +
                              std::to_string(pid));
  }
  return CapturedProcessReference{pid, start_ticks, kind, node_directory};
}

bool CapturedProcessStillAlive(const CapturedProcessReference &process) {
  if (!ProcessAlive(process.pid)) {
    return false;
  }
  try {
    if (ReadProcessStartTicks(process.pid) != process.start_ticks) {
      return false;
    }
    if (!ProcessAlive(process.pid)) {
      return false;
    }
    return ReadProcessStartTicks(process.pid) == process.start_ticks;
  } catch (const std::exception &failure) {
    if (!ProcessAlive(process.pid)) {
      return false;
    }
    throw ProcessCleanupError(
        "Cannot verify termination of captured CCM process " +
        std::to_string(process.pid) + ": " + failure.what());
  }
}

bool CapturedProcessesAreGone(
    const std::vector<CapturedProcessReference> &processes) {
  return std::none_of(
      processes.begin(), processes.end(), CapturedProcessStillAlive);
}

std::vector<CapturedProcessReference> ReferencesForNode(
    const std::vector<CapturedProcessReference> &processes,
    const std::string &node_name) {
  std::vector<CapturedProcessReference> selected;
  std::copy_if(processes.begin(), processes.end(),
               std::back_inserter(selected),
               [&](const CapturedProcessReference &process) {
                 return process.node_directory.filename() == node_name;
               });
  return selected;
}

void AtomicWrite(const fs::path &path, const std::string &contents) {
  const auto temporary =
      path.parent_path() /
      ("." + path.filename().string() + ".tmp-" + std::to_string(::getpid()));
  const int fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error(ErrnoMessage("open " + temporary.string()));
  }
  try {
    WriteAll(fd, contents);
    if (::fsync(fd) != 0) {
      throw std::runtime_error(ErrnoMessage("fsync " + temporary.string()));
    }
  } catch (...) {
    ::close(fd);
    fs::remove(temporary);
    throw;
  }
  if (::close(fd) != 0) {
    fs::remove(temporary);
    throw std::runtime_error(ErrnoMessage("close " + temporary.string()));
  }
  fs::rename(temporary, path);
}

std::vector<std::string> SplitLines(const std::string &contents) {
  std::vector<std::string> lines;
  std::istringstream input(contents);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::size_t Indentation(const std::string &line) {
  std::size_t indentation = 0;
  while (indentation < line.size() && line[indentation] == ' ') {
    ++indentation;
  }
  return indentation;
}

bool IsMappingEntry(const std::string &line, std::size_t indentation,
                    const std::string &key) {
  if (Indentation(line) != indentation ||
      line.size() < indentation + key.size() + 1) {
    return false;
  }
  return line.compare(indentation, key.size(), key) == 0 &&
         line[indentation + key.size()] == ':';
}

bool ExistsWithoutFollowingLinks(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return false;
  }
  if (error) {
    throw std::system_error(error, "Unable to inspect " + path.string());
  }
  return status.type() != fs::file_type::not_found;
}

void ValidateOwnedRegularFile(const fs::path &path,
                              const std::string &description) {
  const auto status = fs::symlink_status(path);
  if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
    throw std::runtime_error("Refusing unsafe " + description + " " +
                             path.string());
  }
  struct stat metadata {};
  if (::lstat(path.c_str(), &metadata) != 0) {
    throw std::runtime_error(ErrnoMessage("lstat " + path.string()));
  }
  if (metadata.st_uid != ::geteuid()) {
    throw std::runtime_error(description +
                             " is not owned by current user: " +
                             path.string());
  }
}

std::string TrimYamlText(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string ParseSafeYamlString(std::string value, const fs::path &source) {
  value = TrimYamlText(std::move(value));
  if (value.size() >= 2 &&
      ((value.front() == '\'' && value.back() == '\'') ||
       (value.front() == '"' && value.back() == '"'))) {
    value = value.substr(1, value.size() - 2);
  }
  if (value.empty() || value.find_first_of("\r\n") != std::string::npos ||
      value.find('\0') != std::string::npos) {
    throw std::runtime_error("Invalid YAML string in " + source.string());
  }
  return value;
}

std::optional<std::pair<std::size_t, std::string>> FindTopLevelYamlValue(
    const std::vector<std::string> &lines, const std::string &key,
    const fs::path &source) {
  std::optional<std::pair<std::size_t, std::string>> result;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (!IsMappingEntry(lines[index], 0, key)) {
      continue;
    }
    if (result) {
      throw std::runtime_error("Duplicate YAML key '" + key + "' in " +
                               source.string());
    }
    const auto colon = lines[index].find(':');
    result = std::make_pair(index, TrimYamlText(lines[index].substr(colon + 1)));
  }
  return result;
}

std::string RequireTopLevelYamlString(const std::vector<std::string> &lines,
                                      const std::string &key,
                                      const fs::path &source) {
  const auto value = FindTopLevelYamlValue(lines, key, source);
  if (!value) {
    throw std::runtime_error("Missing YAML key '" + key + "' in " +
                             source.string());
  }
  return ParseSafeYamlString(value->second, source);
}

std::vector<std::string> ParseInlineYamlStringList(
    const std::string &text, const fs::path &source) {
  if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
    throw std::runtime_error("Invalid YAML list in " + source.string());
  }
  std::vector<std::string> values;
  const auto contents = text.substr(1, text.size() - 2);
  std::size_t begin = 0;
  while (begin < contents.size()) {
    const auto comma = contents.find(',', begin);
    const auto end = comma == std::string::npos ? contents.size() : comma;
    values.push_back(ParseSafeYamlString(contents.substr(begin, end - begin),
                                         source));
    if (comma == std::string::npos) {
      break;
    }
    begin = comma + 1;
  }
  return values;
}

std::vector<std::string> RequireTopLevelYamlStringList(
    const std::vector<std::string> &lines, const std::string &key,
    const fs::path &source) {
  const auto value = FindTopLevelYamlValue(lines, key, source);
  if (!value) {
    throw std::runtime_error("Missing YAML key '" + key + "' in " +
                             source.string());
  }
  if (!value->second.empty()) {
    return ParseInlineYamlStringList(value->second, source);
  }

  std::vector<std::string> values;
  for (std::size_t index = value->first + 1; index < lines.size(); ++index) {
    const auto trimmed = TrimYamlText(lines[index]);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    if (trimmed.rfind("-", 0) != 0) {
      if (Indentation(lines[index]) == 0) {
        break;
      }
      throw std::runtime_error("Invalid YAML list for '" + key + "' in " +
                               source.string());
    }
    if (trimmed.size() < 2 ||
        (trimmed[1] != ' ' && trimmed[1] != '\t')) {
      throw std::runtime_error("Invalid YAML list for '" + key + "' in " +
                               source.string());
    }
    values.push_back(ParseSafeYamlString(trimmed.substr(2), source));
  }
  return values;
}

YAML::Node NormalizeYamlStrings(const YAML::Node &node) {
  if (node.IsMap()) {
    YAML::Node result(YAML::NodeType::Map);
    for (const auto &entry : node) {
      result[NormalizeYamlStrings(entry.first)] =
          NormalizeYamlStrings(entry.second);
    }
    return result;
  }
  if (node.IsSequence()) {
    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto &entry : node) {
      result.push_back(NormalizeYamlStrings(entry));
    }
    return result;
  }
  if (node.IsScalar() && node.Tag() == "!") {
    YAML::Node result(node.Scalar());
    result.SetTag("tag:yaml.org,2002:str");
    return result;
  }
  return YAML::Clone(node);
}

YAML::Node ParseYamlMapping(const fs::path &path,
                            const std::string &description) {
  ValidateOwnedRegularFile(path, description);
  YAML::Node mapping;
  try {
    mapping = YAML::Load(ReadFile(path));
  } catch (const YAML::Exception &failure) {
    throw std::runtime_error("Invalid YAML in " + path.string() + ": " +
                             failure.what());
  }
  if (!mapping || !mapping.IsMap()) {
    throw std::runtime_error(description + " is not a YAML mapping: " +
                             path.string());
  }
  return NormalizeYamlStrings(mapping);
}

YAML::Node ParseYamlOverrideValue(const std::string &value,
                                  const std::string &key) {
  try {
    const auto documents = YAML::LoadAll(value);
    if (documents.size() != 1) {
      throw std::runtime_error("Scylla YAML override '" + key +
                               "' must contain exactly one document");
    }
    return NormalizeYamlStrings(documents.front());
  } catch (const YAML::Exception &failure) {
    throw std::runtime_error("Invalid Scylla YAML override '" + key +
                             "': " + failure.what());
  }
}

void ApplyDottedYamlValue(YAML::Node mapping, const std::string &key,
                          const std::string &value) {
  const auto separator = key.find('.');
  if (separator == std::string::npos) {
    mapping[key] = ParseYamlOverrideValue(value, key);
    return;
  }
  const auto root = key.substr(0, separator);
  const auto child = key.substr(separator + 1);
  auto nested = mapping[root];
  if (!nested || nested.IsNull()) {
    mapping[root] = YAML::Node(YAML::NodeType::Map);
    nested = mapping[root];
  } else if (!nested.IsMap()) {
    throw std::runtime_error("YAML key '" + root + "' is not a mapping");
  }
  nested[child] = ParseYamlOverrideValue(value, key);
}

void WriteYamlMapping(const fs::path &path, const YAML::Node &mapping) {
  YAML::Emitter output;
  output.SetMapFormat(YAML::Block);
  output.SetSeqFormat(YAML::Block);
  output << mapping;
  if (!output.good()) {
    throw std::runtime_error("Unable to serialize YAML mapping " +
                             path.string());
  }
  AtomicWrite(path, std::string(output.c_str()) + '\n');
}

void ValidateNodeMetadataIfPresent(const fs::path &node_directory,
                                   const std::string &expected_name) {
  const auto node_config = node_directory / "node.conf";
  if (!ExistsWithoutFollowingLinks(node_config)) {
    return;
  }
  ValidateOwnedRegularFile(node_config, "CCM node configuration");
  const auto lines = SplitLines(ReadFile(node_config));
  if (RequireTopLevelYamlString(lines, "name", node_config) != expected_name ||
      FindTopLevelYamlValue(lines, "docker_id", node_config)) {
    throw std::runtime_error("Unsafe CCM node configuration " +
                             node_config.string());
  }
}

void ValidateClusterMetadataIfPresent(const fs::path &cluster_directory,
                                      const std::string &expected_name) {
  ValidateInstanceId(expected_name);
  ValidateOwnedDirectory(cluster_directory, "CCM cluster");
  const auto cluster_config = cluster_directory / "cluster.conf";
  if (!ExistsWithoutFollowingLinks(cluster_config)) {
    return;
  }
  ValidateOwnedRegularFile(cluster_config, "CCM cluster configuration");
  const auto lines = SplitLines(ReadFile(cluster_config));
  if (RequireTopLevelYamlString(lines, "name", cluster_config) !=
      expected_name) {
    throw std::runtime_error("CCM cluster configuration has unexpected name: " +
                             cluster_config.string());
  }

  const auto nodes =
      RequireTopLevelYamlStringList(lines, "nodes", cluster_config);
  if (nodes.empty()) {
    throw std::runtime_error("CCM cluster has no nodes in " +
                             cluster_config.string());
  }
  std::set<std::string> node_names;
  for (const auto &node : nodes) {
    if (!IsNodeName(node) || !node_names.insert(node).second) {
      throw std::runtime_error("Unsafe or duplicate node name in " +
                               cluster_config.string() + ": " + node);
    }
    const auto node_directory = cluster_directory / node;
    if (ExistsWithoutFollowingLinks(node_directory)) {
      if (node_directory.parent_path() != cluster_directory) {
        throw std::runtime_error("CCM node escapes cluster directory: " + node);
      }
      ValidateOwnedDirectory(node_directory, "CCM node");
      ValidateNodeMetadataIfPresent(node_directory, node);
    }
  }
  for (const auto &seed :
       RequireTopLevelYamlStringList(lines, "seeds", cluster_config)) {
    if (node_names.count(seed) == 0) {
      throw std::runtime_error("Unsafe seed name in " +
                               cluster_config.string() + ": " + seed);
    }
  }
}

bool ClusterListsNode(const fs::path &cluster_directory,
                      const std::string &expected_cluster_name,
                      const std::string &node_name) {
  ValidateClusterMetadataIfPresent(cluster_directory, expected_cluster_name);
  const auto cluster_config = cluster_directory / "cluster.conf";
  if (!ExistsWithoutFollowingLinks(cluster_config)) {
    throw std::runtime_error("Missing CCM cluster configuration " +
                             cluster_config.string());
  }
  ValidateOwnedRegularFile(cluster_config, "CCM cluster configuration");
  const auto nodes = RequireTopLevelYamlStringList(
      SplitLines(ReadFile(cluster_config)), "nodes", cluster_config);
  return std::find(nodes.begin(), nodes.end(), node_name) != nodes.end();
}

std::optional<fs::path> CurrentClusterDirectory(
    const fs::path &ccm_directory) {
  const auto current_path = ccm_directory / "CURRENT";
  if (!ExistsWithoutFollowingLinks(current_path)) {
    return std::nullopt;
  }
  ValidateOwnedRegularFile(current_path, "CCM CURRENT file");
  const auto current = TrimYamlText(ReadFile(current_path));
  ValidateInstanceId(current);
  const auto cluster_directory = ccm_directory / current;
  if (cluster_directory.parent_path() != ccm_directory) {
    throw std::runtime_error("CCM CURRENT escapes configuration directory");
  }
  if (!ExistsWithoutFollowingLinks(cluster_directory)) {
    throw std::runtime_error("CCM CURRENT references a missing cluster: " +
                             current);
  }
  ValidateClusterMetadataIfPresent(cluster_directory, current);
  return cluster_directory;
}

void ApplyYamlOverrides(const ClusterSpec &spec, const fs::path &ccm_directory,
                        const std::vector<TestClusterNode> &nodes,
                        bool update_cluster_state) {
  if (spec.ScyllaYamlOverrides().empty()) {
    return;
  }
  const auto current_path = ccm_directory / "CURRENT";
  if (!fs::is_regular_file(current_path)) {
    throw std::runtime_error("CCM has no CURRENT cluster under " +
                             ccm_directory.string());
  }
  auto current = ReadFile(current_path);
  while (!current.empty() &&
         std::isspace(static_cast<unsigned char>(current.back()))) {
    current.pop_back();
  }
  ValidateInstanceId(current);
  const auto cluster_directory = ccm_directory / current;
  ValidateClusterMetadataIfPresent(cluster_directory, current);

  if (update_cluster_state) {
    const auto cluster_config = cluster_directory / "cluster.conf";
    auto cluster = ParseYamlMapping(cluster_config, "CCM cluster configuration");
    auto options = cluster["config_options"];
    if (!options || options.IsNull()) {
      cluster["config_options"] = YAML::Node(YAML::NodeType::Map);
      options = cluster["config_options"];
    } else if (!options.IsMap()) {
      throw std::runtime_error("CCM config_options is not a YAML mapping in " +
                               cluster_config.string());
    }
    for (const auto &option : spec.ScyllaYamlOverrides()) {
      ApplyDottedYamlValue(options, option.first, option.second);
    }
    WriteYamlMapping(cluster_config, cluster);
  }

  static const std::set<std::string> implicit_node_keys = {
      "hinted_handoff_enabled",
      "commitlog_sync",
      "commitlog_sync_period_in_ms",
      "commitlog_sync_batch_window_in_ms",
  };
  for (const auto &node : nodes) {
    const auto node_directory = cluster_directory / node.name;
    const auto node_config = node_directory / "node.conf";
    if (fs::is_regular_file(node_config)) {
      auto node = ParseYamlMapping(node_config, "CCM node configuration");
      auto options = node["config_options"];
      bool changed = false;
      for (const auto &option : spec.ScyllaYamlOverrides()) {
        if (implicit_node_keys.count(option.first) != 0 && options &&
            options.IsMap() && options.remove(option.first)) {
          changed = true;
        }
      }
      if (changed) {
        WriteYamlMapping(node_config, node);
      }
    }

    const auto scylla_config = node_directory / "conf" / "scylla.yaml";
    auto scylla = ParseYamlMapping(scylla_config, "Scylla configuration");
    for (const auto &option : spec.ScyllaYamlOverrides()) {
      ApplyDottedYamlValue(scylla, option.first, option.second);
    }
    WriteYamlMapping(scylla_config, scylla);
  }
}

void RemoveNodeConfigPid(const fs::path &node_config) {
  std::istringstream input(ReadFile(node_config));
  std::ostringstream output;
  std::string line;
  bool changed = false;
  while (std::getline(input, line)) {
    if (line.rfind("pid:", 0) == 0) {
      changed = true;
      continue;
    }
    output << line << '\n';
  }
  if (changed) {
    AtomicWrite(node_config, output.str());
  }
}

std::vector<CapturedProcessReference> PrepareProcessReferencesForCcm(
    const fs::path &cluster_directory, const fs::path &run_directory) {
  std::vector<CapturedProcessReference> captured;
  if (!fs::exists(cluster_directory)) {
    return captured;
  }
  ValidateOwnedDirectory(cluster_directory, "CCM cluster");
  ValidateClusterMetadataIfPresent(cluster_directory,
                                   cluster_directory.filename().string());
  for (const auto &entry : fs::directory_iterator(cluster_directory)) {
    if (!IsNodeName(entry.path().filename().string())) {
      continue;
    }
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
      throw ProcessCleanupError("Refusing unsafe CCM node directory " +
                                entry.path().string());
    }
    const auto node_config = entry.path() / "node.conf";
    const auto configured_pid = ReadNodeConfigPid(node_config);
    bool configured_alive = false;
    if (configured_pid) {
      configured_alive = ProcessAlive(*configured_pid);
      if (configured_alive &&
          !ProcessBelongsToNode(*configured_pid, entry.path(), run_directory,
                                ProcessReferenceKind::Scylla)) {
        throw ProcessCleanupError("CCM node references unrelated live PID " +
                                  std::to_string(*configured_pid));
      }
      if (configured_alive) {
        captured.push_back(CaptureProcessReference(
            *configured_pid, entry.path(), run_directory,
            ProcessReferenceKind::Scylla));
      }
      if (!configured_alive) {
        RemoveNodeConfigPid(node_config);
      }
    }

    const auto scylla_pid = ReadPidFile(entry.path() / "cassandra.pid");
    bool scylla_alive = false;
    if (scylla_pid) {
      scylla_alive = ProcessAlive(*scylla_pid);
      if (scylla_alive &&
          !ProcessBelongsToNode(*scylla_pid, entry.path(), run_directory,
                                ProcessReferenceKind::Scylla)) {
        throw ProcessCleanupError("CCM node references unrelated live PID " +
                                  std::to_string(*scylla_pid));
      }
      if (scylla_alive &&
          std::none_of(captured.begin(), captured.end(),
                       [&](const CapturedProcessReference &process) {
                         return process.pid == *scylla_pid &&
                                process.kind == ProcessReferenceKind::Scylla;
                       })) {
        captured.push_back(CaptureProcessReference(
            *scylla_pid, entry.path(), run_directory,
            ProcessReferenceKind::Scylla));
      }
      if (!scylla_alive) {
        fs::remove(entry.path() / "cassandra.pid");
      }
    }
    if (scylla_alive &&
        (!configured_alive || configured_pid != scylla_pid)) {
      throw ProcessCleanupError(
          "CCM cannot safely manage Scylla PID " +
          std::to_string(*scylla_pid) +
          " because node.conf does not reference the same owned process");
    }

    const std::pair<const char *, ProcessReferenceKind> references[] = {
        {"scylla-jmx.pid", ProcessReferenceKind::Jmx},
        {"scylla-agent.pid", ProcessReferenceKind::Agent},
    };
    for (const auto &[name, kind] : references) {
      const auto path = entry.path() / name;
      const auto pid = ReadPidFile(path);
      if (!pid) {
        continue;
      }
      if (ProcessAlive(*pid)) {
        if (!ProcessBelongsToNode(*pid, entry.path(), run_directory, kind)) {
          throw ProcessCleanupError("CCM node references unrelated live PID " +
                                    std::to_string(*pid));
        }
        captured.push_back(CaptureProcessReference(
            *pid, entry.path(), run_directory, kind));
      } else {
        fs::remove(path);
      }
    }
  }
  return captured;
}

bool NodeHasLiveProcessReferences(const fs::path &node_directory) {
  for (const auto pid : ReadNodePids(node_directory)) {
    if (ProcessAlive(pid)) {
      return true;
    }
  }
  for (const auto *name : {"scylla-jmx.pid", "scylla-agent.pid"}) {
    const auto pid = ReadPidFile(node_directory / name);
    if (pid && ProcessAlive(*pid)) {
      return true;
    }
  }
  return false;
}

bool ClusterHasLiveProcessReferences(const fs::path &cluster_directory) {
  if (!fs::exists(cluster_directory)) {
    return false;
  }
  for (const auto &entry : fs::directory_iterator(cluster_directory)) {
    if (IsNodeName(entry.path().filename().string()) &&
        fs::is_directory(entry.symlink_status()) &&
        NodeHasLiveProcessReferences(entry.path())) {
      return true;
    }
  }
  return false;
}

bool IsPrivateKeyFile(const fs::path &path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == ".key" || extension == ".pem";
}

void PrepareDiagnosticDirectoryTree(const fs::path &root,
                                    const fs::path &directory) {
  const auto normalized_root = AbsoluteNormalized(root);
  const auto normalized_directory = AbsoluteNormalized(directory);
  if (!PathStartsWith(normalized_directory, normalized_root)) {
    throw std::runtime_error("Diagnostic path escapes " +
                             normalized_root.string());
  }
  PrepareDirectory(normalized_root, "CCM diagnostics");
  auto current = normalized_root;
  const auto relative =
      normalized_directory.lexically_relative(normalized_root);
  for (const auto &component : relative) {
    if (component.empty() || component == ".") {
      continue;
    }
    current /= component;
    if (!fs::exists(current)) {
      fs::create_directory(current);
    }
    ValidateOwnedDirectory(current, "CCM diagnostic");
  }
}

void CopyDiagnosticFile(const fs::path &source,
                        const fs::path &destination_root,
                        const fs::path &relative) {
  if (!fs::exists(source)) {
    return;
  }
  const auto source_status = fs::symlink_status(source);
  if (fs::is_symlink(source_status) || !fs::is_regular_file(source_status)) {
    throw std::runtime_error("Refusing unsafe diagnostic source " +
                             source.string());
  }
  const auto target = AbsoluteNormalized(destination_root / relative);
  if (!PathStartsWith(target, AbsoluteNormalized(destination_root)) ||
      target == destination_root) {
    throw std::runtime_error("Diagnostic target escapes " +
                             destination_root.string());
  }
  PrepareDiagnosticDirectoryTree(destination_root, target.parent_path());
  if (fs::exists(target)) {
    const auto target_status = fs::symlink_status(target);
    if (fs::is_symlink(target_status) || !fs::is_regular_file(target_status)) {
      throw std::runtime_error("Refusing unsafe diagnostic target " +
                               target.string());
    }
  }
  const auto temporary =
      target.parent_path() / (".ccm-diagnostic-" + std::to_string(::getpid()) +
                              '-' + target.filename().string());
  fs::remove(temporary);
  fs::copy_file(source, temporary, fs::copy_options::overwrite_existing);
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);
  fs::rename(temporary, target);
}

void CopyDiagnosticLogTree(const fs::path &logs_directory,
                           const fs::path &destination,
                           const fs::path &relative) {
  if (!fs::exists(logs_directory)) {
    return;
  }
  ValidateOwnedDirectory(logs_directory, "CCM logs");
  for (const auto &entry : fs::recursive_directory_iterator(logs_directory)) {
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status)) {
      throw std::runtime_error("Refusing symbolic link in CCM logs: " +
                               entry.path().string());
    }
    if (fs::is_regular_file(status) && !IsPrivateKeyFile(entry.path())) {
      CopyDiagnosticFile(entry.path(), destination,
                         relative / fs::relative(entry.path(), logs_directory));
    }
  }
}

void RemoveTreeSafely(const fs::path &root) {
  if (!ExistsWithoutFollowingLinks(root)) {
    return;
  }
  const auto root_status = fs::symlink_status(root);
  if (fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
    throw std::runtime_error("Refusing unsafe recursive cleanup target " +
                             root.string());
  }
  ValidateOwnedDirectory(root, "recursive cleanup target");
  // std::filesystem::remove_all unlinks contained symlinks without following
  // them, which is required for CCM's node-local executable symlinks.
  fs::remove_all(root);
}

void CleanupCurrentPointer(const fs::path &ccm_directory,
                           const std::string &instance_id) {
  const auto current = ccm_directory / "CURRENT";
  if (!fs::exists(current)) {
    return;
  }
  const auto status = fs::symlink_status(current);
  if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
    throw std::runtime_error("Refusing unsafe CCM CURRENT file " +
                             current.string());
  }
  std::string value = ReadFile(current);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  if (value == instance_id) {
    fs::remove(current);
  }
}

bool ExceptionIsProcessCleanup(const std::exception_ptr &failure) {
  if (!failure) {
    return false;
  }
  try {
    std::rethrow_exception(failure);
  } catch (const ProcessCleanupError &) {
    return true;
  } catch (...) {
    return false;
  }
}

std::string ExceptionMessage(const std::exception_ptr &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return "unknown non-standard exception";
  }
}

} // namespace

CcmProvisioner::CcmProvisioner(const fs::path &run_directory)
    : CcmProvisioner(run_directory,
                     ValidateOperationalDiagnostics(
                         run_directory, ConfiguredDiagnosticsDirectory()),
                     GetEnvironment("SCYLLA_CCM_PATH", "ccm"),
                     std::chrono::minutes(10)) {}

CcmProvisioner::CcmProvisioner(const fs::path &run_directory,
                               fs::path diagnostics_directory,
                               std::string ccm_executable,
                               std::chrono::seconds command_timeout)
    : run_directory_(AbsoluteNormalized(run_directory)),
      clusters_directory_(run_directory_ / "clusters"),
      diagnostics_directory_(
          ValidateDiagnosticsSeparation(run_directory_, diagnostics_directory)),
      ccm_executable_(std::move(ccm_executable)),
      command_timeout_(command_timeout) {
  ValidateArgument(ccm_executable_, "CCM executable");
  if (command_timeout_ <= std::chrono::seconds::zero()) {
    throw std::invalid_argument("CCM command timeout must be positive");
  }
  PrepareDirectory(run_directory_, "CCM run");
  if (!fs::exists(clusters_directory_)) {
    fs::create_directory(clusters_directory_);
  }
  ValidateOwnedDirectory(clusters_directory_, "CCM clusters");
  SetOwnerOnlyDirectoryPermissions(clusters_directory_);
  PrepareDirectory(diagnostics_directory_, "CCM diagnostics");
}

fs::path CcmProvisioner::RunDirectory() const { return run_directory_; }

bool CcmProvisioner::RequiresJmxPortReservation(const ClusterSpec &spec) const {
  if (spec.ScyllaVersion() != ClusterSpec::kDefaultScyllaVersion) {
    return true;
  }
  try {
    auto executable = AbsoluteNormalized(ccm_executable_);
    if (!fs::exists(executable) || !fs::is_regular_file(executable)) {
      return true;
    }
    executable = fs::canonical(executable);
    const auto environment = executable.parent_path().parent_path();
    const auto expected_name = "scylla-ccm-" + std::string(kPinnedCcmCommit);
    const auto marker = environment / ".install-complete";
    if (environment.filename() != expected_name ||
        executable != fs::canonical(environment / "bin" / "ccm") ||
        !fs::is_regular_file(marker) ||
        fs::is_symlink(fs::symlink_status(marker))) {
      return true;
    }
    auto value = ReadFile(marker);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
    }
    return value != kPinnedCcmCommit;
  } catch (...) {
    return true;
  }
}

ProvisionedClusterData CcmProvisioner::Provision(const ClusterSpec &spec,
                                                 const std::string &instance_id,
                                                 int ccm_id) {
  spec.Validate();
  ValidateInstanceId(instance_id);
  ValidateCcmId(ccm_id);

  const auto ccm_directory = clusters_directory_ / instance_id;
  if (!fs::exists(ccm_directory)) {
    PrepareDirectChildDirectory(clusters_directory_, ccm_directory,
                                "CCM configuration");
  } else {
    ValidateOwnedDirectory(ccm_directory, "CCM configuration");
    SetOwnerOnlyDirectoryPermissions(ccm_directory);
  }

  auto nodes = BuildNodes(spec.Topology(), ccm_id);
  fs::path ca_certificate_path;
  std::optional<Credentials> credentials;

  try {
    if (HasTransport(spec, AlternatorTransport::Https)) {
      const auto tls_directory = ccm_directory / "tls";
      PrepareDirectory(tls_directory, "CCM TLS");
      ca_certificate_path = tls_directory / "ca.crt";
      RunCommand(ccm_directory, {
                                    "openssl",
                                    "req",
                                    "-x509",
                                    "-newkey",
                                    "rsa:3072",
                                    "-sha256",
                                    "-nodes",
                                    "-days",
                                    "730",
                                    "-subj",
                                    "/CN=" + instance_id + " test CA",
                                    "-keyout",
                                    (tls_directory / "ca.key").string(),
                                    "-out",
                                    ca_certificate_path.string(),
                                });
    }

    if (spec.Security().EnforceAlternatorAuthorization()) {
      credentials =
          Credentials{std::string(kTestUser), std::string(kTestSaltedPassword)};
    }

    std::vector<std::string> create_arguments = {
        "create",
        "--config-dir",
        ccm_directory.string(),
        instance_id,
        "--scylla",
        "--version",
        spec.ScyllaVersion(),
        "--nodes",
        FirstRackCounts(spec.Topology()),
        "--id",
        std::to_string(ccm_id),
    };
    if (spec.Topology().Datacenters().size() > 1) {
      create_arguments.push_back("--snitch");
      create_arguments.emplace_back(kGossipingPropertyFileSnitch);
    }
    RunCcm(ccm_directory, create_arguments);

    int first_rack_nodes = 0;
    for (const auto &datacenter : spec.Topology().Datacenters()) {
      first_rack_nodes += datacenter.Racks().front().NodeCount();
    }
    for (std::size_t index = static_cast<std::size_t>(first_rack_nodes);
         index < nodes.size(); ++index) {
      const auto &node = nodes[index];
      RunCcm(ccm_directory, {
                                "add",
                                "--config-dir",
                                ccm_directory.string(),
                                node.name,
                                "--scylla",
                                "--seeds",
                                "--itf",
                                node.address,
                                "--data-center",
                                node.datacenter,
                                "--rack",
                                node.rack,
                            });
    }

    std::map<std::string, std::string> options;
    options["alternator_write_isolation"] = "only_rmw_uses_lwt";
    options["endpoint_snitch"] = std::string(kGossipingPropertyFileSnitch);
    options["start_native_transport"] = "true";
    if (HasTransport(spec, AlternatorTransport::Http)) {
      options["alternator_port"] = std::to_string(kHttpPort);
    }
    if (HasTransport(spec, AlternatorTransport::Https)) {
      options["alternator_https_port"] = std::to_string(kHttpsPort);
      options["alternator_encryption_options.enable_session_tickets"] = "true";
    }
    if (spec.Security().Authentication() != AuthenticationMode::AllowAll) {
      options["authenticator"] =
          Authenticator(spec.Security().Authentication());
      options["auth_superuser_name"] = std::string(kTestUser);
      options["auth_superuser_salted_password"] =
          std::string(kTestSaltedPassword);
    }
    if (spec.Security().Authorization() != AuthorizationMode::AllowAll) {
      options["authorizer"] = Authorizer(spec.Security().Authorization());
    }
    if (spec.Security().EnforceAlternatorAuthorization()) {
      options["alternator_enforce_authorization"] = "true";
    }
    options.insert(spec.ScyllaYamlOverrides().begin(),
                   spec.ScyllaYamlOverrides().end());

    std::vector<std::string> update_arguments = {
        "updateconf",
        "--config-dir",
        ccm_directory.string(),
    };
    for (const auto &option : options) {
      update_arguments.push_back(option.first + ':' + option.second);
    }
    RunCcm(ccm_directory, update_arguments);
    ApplyYamlOverrides(spec, ccm_directory, nodes, true);

    if (!ca_certificate_path.empty()) {
      for (const auto &node : nodes) {
        ConfigureNodeCertificate(node, ccm_directory, ca_certificate_path);
      }
    }
    ApplyYamlOverrides(spec, ccm_directory, nodes, false);

    for (const auto &node : nodes) {
      RunCcm(ccm_directory, StartArguments(spec, ccm_directory, node.name));
    }
    WaitForEndpoints(spec, nodes, ca_certificate_path, kReadinessTimeout);

    return ProvisionedClusterData{
        ccm_id,
        ccm_directory,
        std::move(nodes),
        std::move(ca_certificate_path),
        std::move(credentials),
    };
  } catch (...) {
    const auto original = std::current_exception();
    const auto original_message = ExceptionMessage(original);
    std::string diagnostic_message;
    try {
      CollectDiagnostics(instance_id, ccm_directory);
    } catch (const std::exception &diagnostic_failure) {
      diagnostic_message = diagnostic_failure.what();
    }
    if (ExceptionIsProcessCleanup(original)) {
      throw ClusterProvisioningError(
          "Failed to provision '" + instance_id +
              "'; command process cleanup is unproven: " + original_message +
              (diagnostic_message.empty()
                   ? std::string()
                   : "; diagnostic snapshot failed: " + diagnostic_message),
          false,
          true);
    }
    if (!diagnostic_message.empty()) {
      throw ClusterProvisioningError(
          "Failed to provision '" + instance_id + "': " + original_message +
              "; diagnostic snapshot failed: " + diagnostic_message,
          false);
    }
    try {
      RemoveByName(instance_id, ccm_directory);
    } catch (const std::exception &rollback_failure) {
      throw ClusterProvisioningError(
          "Failed to provision '" + instance_id + "': " + original_message +
              "; CCM rollback failed: " + rollback_failure.what(),
          false,
          dynamic_cast<const RecoveryRequiredError *>(&rollback_failure) !=
              nullptr);
    }
    throw ClusterProvisioningError(
        "Failed to provision '" + instance_id +
            "'; CCM rollback succeeded: " + original_message,
        true);
  }
}

void CcmProvisioner::Start(const PhysicalTestCluster &cluster) {
  const auto cluster_directory =
      cluster.CcmDirectory() / cluster.InstanceId();
  PrepareProcessReferencesForCcm(cluster_directory, run_directory_);
  for (const auto &node : cluster.Nodes()) {
    if (!IsNodeRunning(cluster, node)) {
      const auto node_directory = cluster_directory / node.name;
      if (NodeHasLiveProcessReferences(node_directory)) {
        throw ProcessCleanupError(
            "Cannot start CCM node '" + node.name +
            "' while an ancillary node process remains alive");
      }
      RunCcm(cluster.CcmDirectory(),
             StartArguments(cluster.Spec(), cluster.CcmDirectory(), node.name));
    }
  }
  WaitForAlternator(cluster, cluster.Nodes(), kReadinessTimeout);
}

void CcmProvisioner::Stop(const PhysicalTestCluster &cluster) {
  const auto cluster_directory =
      cluster.CcmDirectory() / cluster.InstanceId();
  const auto captured =
      PrepareProcessReferencesForCcm(cluster_directory, run_directory_);
  std::exception_ptr command_failure;
  try {
    RunCcm(cluster.CcmDirectory(),
           {"stop", "--config-dir", cluster.CcmDirectory().string()});
  } catch (...) {
    command_failure = std::current_exception();
  }

  const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const bool captured_gone = CapturedProcessesAreGone(captured);
    (void)PrepareProcessReferencesForCcm(cluster_directory, run_directory_);
    if (captured_gone &&
        !ClusterHasLiveProcessReferences(cluster_directory)) {
      if (command_failure && ExceptionIsProcessCleanup(command_failure)) {
        std::rethrow_exception(command_failure);
      }
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (command_failure) {
    std::rethrow_exception(command_failure);
  }
  throw ProcessCleanupError("CCM cluster stop left node processes alive");
}

void CcmProvisioner::StartNode(const PhysicalTestCluster &cluster,
                               const TestClusterNode &node) {
  const auto cluster_directory =
      cluster.CcmDirectory() / cluster.InstanceId();
  PrepareProcessReferencesForCcm(cluster_directory, run_directory_);
  if (!IsNodeRunning(cluster, node)) {
    const auto node_directory = cluster_directory / node.name;
    if (NodeHasLiveProcessReferences(node_directory)) {
      throw ProcessCleanupError(
          "Cannot start CCM node '" + node.name +
          "' while an ancillary node process remains alive");
    }
    RunCcm(cluster.CcmDirectory(),
           StartArguments(cluster.Spec(), cluster.CcmDirectory(), node.name));
  }
  WaitForAlternator(cluster, {node}, kReadinessTimeout);
}

void CcmProvisioner::StopNode(const PhysicalTestCluster &cluster,
                              const TestClusterNode &node) {
  const auto cluster_directory =
      cluster.CcmDirectory() / cluster.InstanceId();
  const auto captured = ReferencesForNode(
      PrepareProcessReferencesForCcm(cluster_directory, run_directory_),
      node.name);
  std::exception_ptr command_failure;
  try {
    RunCcm(cluster.CcmDirectory(), {node.name, "stop", "--config-dir",
                                    cluster.CcmDirectory().string()});
  } catch (...) {
    command_failure = std::current_exception();
  }

  const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const bool captured_gone = CapturedProcessesAreGone(captured);
    (void)PrepareProcessReferencesForCcm(cluster_directory, run_directory_);
    if (captured_gone &&
        !NodeHasLiveProcessReferences(cluster_directory / node.name)) {
      if (command_failure && ExceptionIsProcessCleanup(command_failure)) {
        std::rethrow_exception(command_failure);
      }
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (command_failure) {
    std::rethrow_exception(command_failure);
  }
  throw ProcessCleanupError("CCM node stop left process alive: " + node.name);
}

TestClusterNode CcmProvisioner::AddNode(const PhysicalTestCluster &cluster,
                                        const std::string &datacenter,
                                        const std::string &rack) {
  ValidateArgument(datacenter, "Datacenter");
  ValidateArgument(rack, "Rack");
  const auto existing = cluster.Nodes();
  if (existing.size() >=
      static_cast<std::size_t>(ClusterSpec::kMaximumNodeCount)) {
    throw std::runtime_error("A cluster cannot exceed " +
                             std::to_string(ClusterSpec::kMaximumNodeCount) +
                             " nodes");
  }

  int index = 1;
  while (std::any_of(existing.begin(), existing.end(),
                     [&](const TestClusterNode &candidate) {
                       return candidate.name == "node" + std::to_string(index);
                     })) {
    ++index;
  }
  TestClusterNode node{
      "node" + std::to_string(index),
      "127.0." + std::to_string(cluster.CcmId()) + "." + std::to_string(index),
      datacenter,
      rack,
  };
  const auto spec = cluster.Spec();
  bool start_attempted = false;

  try {
    RunCcm(cluster.CcmDirectory(), {
                                       "add",
                                       "--config-dir",
                                       cluster.CcmDirectory().string(),
                                       node.name,
                                       "--scylla",
                                       "--seeds",
                                       "--auto-bootstrap",
                                       "--itf",
                                       node.address,
                                       "--data-center",
                                       node.datacenter,
                                       "--rack",
                                       node.rack,
                                   });
    if (!cluster.CaCertificatePath().empty()) {
      ConfigureNodeCertificate(node, cluster.CcmDirectory(),
                               cluster.CaCertificatePath());
    }
    ApplyYamlOverrides(spec, cluster.CcmDirectory(), {node}, false);
    start_attempted = true;
    RunCcm(cluster.CcmDirectory(),
           StartArguments(spec, cluster.CcmDirectory(), node.name));
    WaitForAlternator(cluster, {node}, kReadinessTimeout);
    return node;
  } catch (...) {
    const auto original = std::current_exception();
    const auto original_message = ExceptionMessage(original);
    std::string diagnostic_message;
    try {
      CollectDiagnostics(cluster.InstanceId(), cluster.CcmDirectory());
    } catch (const std::exception &diagnostic_failure) {
      diagnostic_message = diagnostic_failure.what();
    }
    if (ExceptionIsProcessCleanup(original)) {
      throw NodeProvisioningError(
          "Failed to add '" + node.name +
          "'; command process cleanup is unproven: " + original_message +
          (diagnostic_message.empty()
               ? std::string()
               : "; diagnostic snapshot failed: " + diagnostic_message),
          false,
          true);
    }
    if (!diagnostic_message.empty()) {
      throw NodeProvisioningError(
          "Failed to add '" + node.name + "': " + original_message +
              "; diagnostic snapshot failed: " + diagnostic_message,
          false);
    }
    try {
      const auto cluster_directory =
          cluster.CcmDirectory() / cluster.InstanceId();
      const auto node_directory =
          cluster_directory / node.name;
      const bool directory_exists = ExistsWithoutFollowingLinks(node_directory);
      const bool cluster_lists_node = ClusterListsNode(
          cluster_directory, cluster.InstanceId(), node.name);
      if (directory_exists || cluster_lists_node) {
        const auto captured = ReferencesForNode(
            PrepareProcessReferencesForCcm(cluster_directory, run_directory_),
            node.name);
        if (cluster_lists_node) {
          RunCcm(cluster.CcmDirectory(), {node.name, "remove", "--config-dir",
                                          cluster.CcmDirectory().string()});
        } else {
          if (node_directory.parent_path() != cluster_directory) {
            throw std::runtime_error(
                "Refusing partial CCM node outside cluster directory: " +
                node.name);
          }
          ValidateOwnedDirectory(node_directory, "partial CCM node");
          ValidateNodeMetadataIfPresent(node_directory, node.name);
          if (!CapturedProcessesAreGone(captured)) {
            throw ProcessCleanupError(
                "Partial CCM node rollback found a live process: " +
                node.name);
          }
          RemoveTreeSafely(node_directory);
        }
        if (!CapturedProcessesAreGone(captured)) {
          throw ProcessCleanupError(
              "CCM node rollback left captured processes alive: " +
              node.name);
        }
        if (ExistsWithoutFollowingLinks(node_directory) ||
            ClusterListsNode(cluster_directory, cluster.InstanceId(),
                             node.name)) {
          throw std::runtime_error(
              "CCM node rollback left directory or cluster membership: " +
              node.name);
        }
      }
    } catch (const std::exception &rollback_failure) {
      throw NodeProvisioningError(
          "Failed to add '" + node.name + "': " + original_message +
              "; CCM rollback failed: " + rollback_failure.what(),
          false,
          dynamic_cast<const RecoveryRequiredError *>(&rollback_failure) !=
              nullptr);
    }
    throw NodeProvisioningError(
        "Failed to add '" + node.name +
            "'; CCM rollback succeeded: " + original_message,
        !start_attempted);
  }
}

void CcmProvisioner::DecommissionNode(const PhysicalTestCluster &cluster,
                                      const TestClusterNode &node) {
  RunCcm(cluster.CcmDirectory(), {node.name, "decommission", "--config-dir",
                                  cluster.CcmDirectory().string()});
}

void CcmProvisioner::DeleteNodeState(const PhysicalTestCluster &cluster,
                                     const TestClusterNode &node) {
  CollectDiagnostics(cluster.InstanceId(), cluster.CcmDirectory());
  const auto cluster_directory =
      cluster.CcmDirectory() / cluster.InstanceId();
  const auto node_directory = cluster_directory / node.name;
  const auto inspect_node_state = [&]() {
    return std::pair{
        ExistsWithoutFollowingLinks(node_directory),
        ClusterListsNode(cluster_directory, cluster.InstanceId(), node.name)};
  };
  const auto initial_state = inspect_node_state();
  if (!initial_state.first && !initial_state.second) {
    return;
  }
  if (initial_state.first != initial_state.second) {
    throw std::runtime_error(
        "CCM node directory and cluster membership disagree before removal: " +
        node.name);
  }
  const auto captured = ReferencesForNode(
      PrepareProcessReferencesForCcm(cluster_directory, run_directory_),
      node.name);
  std::exception_ptr command_failure;
  try {
    RunCcm(cluster.CcmDirectory(), {node.name, "remove", "--config-dir",
                                    cluster.CcmDirectory().string()});
  } catch (...) {
    command_failure = std::current_exception();
  }
  if (command_failure && ExceptionIsProcessCleanup(command_failure)) {
    std::rethrow_exception(command_failure);
  }
  if (!CapturedProcessesAreGone(captured)) {
    throw ProcessCleanupError(
        "CCM node removal left captured processes alive: " + node.name);
  }
  const auto final_state = inspect_node_state();
  if (!final_state.first && !final_state.second) {
    return;
  }
  if (command_failure) {
    std::rethrow_exception(command_failure);
  }
  if (final_state.first != final_state.second) {
    throw std::runtime_error(
        "CCM reported success but node directory and cluster membership "
        "disagree: " +
        node.name);
  }
  throw std::runtime_error("CCM reported success but node state remains: " +
                           node.name);
}

bool CcmProvisioner::IsHealthy(const PhysicalTestCluster &cluster) {
  try {
    WaitForAlternator(cluster, cluster.Nodes(), kHealthTimeout);
    return true;
  } catch (...) {
    return false;
  }
}

bool CcmProvisioner::IsNodeRunning(const PhysicalTestCluster &cluster,
                                   const TestClusterNode &node) {
  const auto node_directory =
      cluster.CcmDirectory() / cluster.InstanceId() / node.name;
  if (!fs::exists(node_directory)) {
    return false;
  }
  ValidateOwnedDirectory(node_directory, "CCM node");
  for (const auto pid : ReadNodePids(node_directory)) {
    if (!ProcessAlive(pid)) {
      continue;
    }
    if (!ProcessBelongsToNode(pid, node_directory, run_directory_,
                              ProcessReferenceKind::Scylla)) {
      throw ProcessCleanupError("CCM node '" + node.name +
                                "' references unrelated live PID " +
                                std::to_string(pid));
    }
    return true;
  }
  return false;
}

void CcmProvisioner::WaitForAlternator(
    const PhysicalTestCluster &cluster,
    const std::vector<TestClusterNode> &nodes,
    std::chrono::seconds timeout) const {
  WaitForEndpoints(cluster.Spec(), nodes, cluster.CaCertificatePath(), timeout);
}

void CcmProvisioner::Remove(const PhysicalTestCluster &cluster) {
  CollectDiagnostics(cluster.InstanceId(), cluster.CcmDirectory());
  RemoveByName(cluster.InstanceId(), cluster.CcmDirectory());
}

void CcmProvisioner::ConfigureNodeCertificate(
    const TestClusterNode &node, const fs::path &ccm_directory,
    const fs::path &ca_certificate_path) const {
  const auto tls_directory = ccm_directory / "tls";
  const auto node_directory = tls_directory / node.name;
  PrepareDirectory(node_directory, "CCM node TLS");
  const auto certificate = node_directory / "server.crt";
  const auto key = node_directory / "server.key";
  const auto request = node_directory / "server.csr";
  const auto extension = node_directory / "server.ext";
  {
    std::ofstream output(extension, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Unable to write " + extension.string());
    }
    output << "basicConstraints=critical,CA:FALSE\n"
           << "keyUsage=critical,digitalSignature,keyEncipherment\n"
           << "extendedKeyUsage=serverAuth\n"
           << "subjectAltName=IP:" << node.address << '\n';
    if (!output) {
      throw std::runtime_error("Unable to write " + extension.string());
    }
  }

  RunCommand(ccm_directory, {
                                "openssl",
                                "req",
                                "-newkey",
                                "rsa:3072",
                                "-sha256",
                                "-nodes",
                                "-subj",
                                "/CN=" + node.name,
                                "-keyout",
                                key.string(),
                                "-out",
                                request.string(),
                            });
  RunCommand(ccm_directory, {
                                "openssl",
                                "x509",
                                "-req",
                                "-in",
                                request.string(),
                                "-CA",
                                ca_certificate_path.string(),
                                "-CAkey",
                                (tls_directory / "ca.key").string(),
                                "-CAcreateserial",
                                "-days",
                                "365",
                                "-sha256",
                                "-extfile",
                                extension.string(),
                                "-out",
                                certificate.string(),
                            });
  RunCcm(
      ccm_directory,
      {
          node.name,
          "updateconf",
          "--config-dir",
          ccm_directory.string(),
          "alternator_encryption_options.certificate:" + certificate.string(),
          "alternator_encryption_options.keyfile:" + key.string(),
      });
}

void CcmProvisioner::CollectDiagnostics(const std::string &instance_id,
                                        const fs::path &ccm_directory) const {
  ValidateInstanceId(instance_id);
  if (!fs::exists(ccm_directory)) {
    return;
  }
  ValidateOwnedDirectory(ccm_directory, "CCM configuration");
  const auto destination = diagnostics_directory_ / instance_id;
  PrepareDiagnosticDirectoryTree(diagnostics_directory_, destination);

  for (const auto &entry : fs::directory_iterator(ccm_directory)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() &&
        (name == "ccm-commands.log" || (name.rfind("ccm-command-", 0) == 0 &&
                                        entry.path().extension() == ".log"))) {
      CopyDiagnosticFile(entry.path(), destination, name);
    }
  }

  const auto cluster_directory = ccm_directory / instance_id;
  if (!fs::exists(cluster_directory)) {
    return;
  }
  ValidateOwnedDirectory(cluster_directory, "CCM cluster");
  CopyDiagnosticFile(cluster_directory / "cluster.conf", destination,
                     fs::path(instance_id) / "cluster.conf");

  for (const auto &entry : fs::directory_iterator(cluster_directory)) {
    const auto node_name = entry.path().filename().string();
    if (!IsNodeName(node_name)) {
      continue;
    }
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
      throw std::runtime_error("Refusing unsafe CCM node directory " +
                               entry.path().string());
    }
    const auto node_relative = fs::path(instance_id) / node_name;
    CopyDiagnosticFile(entry.path() / "node.conf", destination,
                       node_relative / "node.conf");
    const auto configuration_directory = entry.path() / "conf";
    if (ExistsWithoutFollowingLinks(configuration_directory)) {
      ValidateOwnedDirectory(configuration_directory,
                             "CCM node configuration");
      CopyDiagnosticFile(configuration_directory / "scylla.yaml", destination,
                         node_relative / "conf" / "scylla.yaml");
    }
    CopyDiagnosticLogTree(entry.path() / "logs", destination,
                          node_relative / "logs");
  }
}

void CcmProvisioner::RemoveByName(const std::string &instance_id,
                                  const fs::path &ccm_directory) const {
  ValidateInstanceId(instance_id);
  const auto normalized_ccm_directory = AbsoluteNormalized(ccm_directory);
  if (normalized_ccm_directory != clusters_directory_ / instance_id) {
    throw std::runtime_error("Refusing CCM configuration outside harness run: " +
                             normalized_ccm_directory.string());
  }
  if (!fs::exists(ccm_directory)) {
    return;
  }
  ValidateOwnedDirectory(ccm_directory, "CCM configuration");
  const auto cluster_directory = ccm_directory / instance_id;
  const auto cluster_config = cluster_directory / "cluster.conf";
  if (!fs::exists(cluster_config)) {
    if (fs::exists(cluster_directory)) {
      RemoveTreeSafely(cluster_directory);
    }
    CleanupCurrentPointer(ccm_directory, instance_id);
    return;
  }
  if (fs::is_symlink(fs::symlink_status(cluster_config)) ||
      !fs::is_regular_file(cluster_config)) {
    throw std::runtime_error("Refusing unsafe CCM cluster configuration " +
                             cluster_config.string());
  }
  ValidateClusterMetadataIfPresent(cluster_directory, instance_id);
  const auto captured =
      PrepareProcessReferencesForCcm(cluster_directory, run_directory_);

  std::exception_ptr command_failure;
  try {
    RunCcm(ccm_directory,
           {"remove", "--config-dir", ccm_directory.string(), instance_id});
  } catch (...) {
    command_failure = std::current_exception();
  }

  if (command_failure && ExceptionIsProcessCleanup(command_failure)) {
    std::rethrow_exception(command_failure);
  }
  if (!CapturedProcessesAreGone(captured)) {
    throw ProcessCleanupError(
        "CCM cluster removal left captured processes alive: " + instance_id);
  }
  const bool removed = !fs::exists(cluster_config);
  if (removed) {
    if (fs::exists(cluster_directory)) {
      RemoveTreeSafely(cluster_directory);
    }
    CleanupCurrentPointer(ccm_directory, instance_id);
    return;
  }
  if (command_failure) {
    std::rethrow_exception(command_failure);
  }
  throw std::runtime_error("CCM reported success but cluster '" + instance_id +
                           "' still exists");
}

void CcmProvisioner::RunCcm(const fs::path &ccm_directory,
                            const std::vector<std::string> &arguments) const {
  if (arguments.empty()) {
    throw std::invalid_argument("Cannot run an empty CCM command");
  }
  if (arguments.front() != "create") {
    (void)CurrentClusterDirectory(ccm_directory);
  }
  std::vector<std::string> command;
  command.reserve(arguments.size() + 1);
  command.push_back(ccm_executable_);
  command.insert(command.end(), arguments.begin(), arguments.end());
  RunCommand(ccm_directory, command);
}

void CcmProvisioner::RunCommand(const fs::path &ccm_directory,
                                const std::vector<std::string> &command) const {
  if (command.empty()) {
    throw std::invalid_argument("Cannot run an empty command");
  }
  ValidateOwnedDirectory(ccm_directory, "CCM configuration");
  for (const auto &argument : command) {
    if (argument.find('\0') != std::string::npos) {
      throw std::invalid_argument("Command argument contains a NUL byte");
    }
  }

  const auto command_text = FormatCommand(command);
  std::vector<std::string> launched_command;
  launched_command.reserve(command.size() + 1);
  launched_command.emplace_back("setsid");
  launched_command.insert(launched_command.end(), command.begin(), command.end());
  auto arguments = MutablePointers(&launched_command);
  auto child_environment = ChildEnvironment(run_directory_);
  auto environment = MutablePointers(&child_environment);

  int output_fd = -1;
  const auto output_path = CreateCommandLog(
      ccm_directory,
      command_counter_.fetch_add(1000, std::memory_order_relaxed), command_text,
      &output_fd);

  posix_spawn_file_actions_t file_actions;
  int spawn_error = ::posix_spawn_file_actions_init(&file_actions);
  const bool file_actions_initialized = spawn_error == 0;
  if (spawn_error == 0) {
    spawn_error =
        ::posix_spawn_file_actions_adddup2(&file_actions, output_fd, STDOUT_FILENO);
  }
  if (spawn_error == 0) {
    spawn_error =
        ::posix_spawn_file_actions_adddup2(&file_actions, output_fd, STDERR_FILENO);
  }
  if (spawn_error == 0 && output_fd > STDERR_FILENO) {
    spawn_error = ::posix_spawn_file_actions_addclose(&file_actions, output_fd);
  }

  pid_t child = -1;
  if (spawn_error == 0) {
    spawn_error = ::posix_spawnp(&child, "setsid", &file_actions, nullptr,
                                 arguments.data(), environment.data());
  }
  const int destroy_error = file_actions_initialized
                                ? ::posix_spawn_file_actions_destroy(&file_actions)
                                : 0;
  if (spawn_error == 0 && destroy_error != 0) {
    spawn_error = destroy_error;
  }
  if (spawn_error != 0) {
    const auto message = std::string("posix_spawnp setsid: ") +
                         std::strerror(spawn_error);
    ::close(output_fd);
    if (child > 0) {
      int status = 0;
      bool reaped = false;
      try {
        TerminateProcessGroup(child, &status, &reaped);
      } catch (...) {
        AppendCommandOutcome(output_path, "start cleanup failed");
        AppendAggregateLog(ccm_directory, output_path);
        throw ProcessCleanupError(message +
                                  "; command process cleanup also failed");
      }
    }
    AppendCommandOutcome(output_path, "start failed");
    AppendAggregateLog(ccm_directory, output_path);
    throw std::runtime_error(message);
  }
  if (::close(output_fd) != 0) {
    const auto close_failure = ErrnoMessage("close command log");
    int status = 0;
    bool reaped = false;
    try {
      TerminateProcessGroup(child, &status, &reaped);
    } catch (...) {
      throw ProcessCleanupError(close_failure +
                                "; command process cleanup also failed");
    }
    throw std::runtime_error(close_failure);
  }

  int status = 0;
  bool reaped = false;
  const auto deadline = std::chrono::steady_clock::now() + command_timeout_;
  while (std::chrono::steady_clock::now() < deadline &&
         !ReapChildNonBlocking(child, &status, &reaped)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const bool timed_out = !reaped;
  std::exception_ptr cleanup_failure;
  if (timed_out || !IsSuccessfulExit(status)) {
    try {
      TerminateProcessGroup(child, &status, &reaped);
    } catch (...) {
      cleanup_failure = std::current_exception();
    }
  }

  const auto outcome = ExitDescription(status, timed_out);
  std::exception_ptr log_failure;
  try {
    AppendCommandOutcome(output_path, outcome);
    AppendAggregateLog(ccm_directory, output_path);
  } catch (...) {
    log_failure = std::current_exception();
  }
  const auto output = ReadFile(output_path);
  std::cout << output;

  if (cleanup_failure) {
    throw ProcessCleanupError(ExceptionMessage(cleanup_failure) +
                              (log_failure
                                   ? "; command log finalization failed: " +
                                         ExceptionMessage(log_failure)
                                   : std::string()));
  }
  if (timed_out || !IsSuccessfulExit(status)) {
    throw CommandError("CCM command failed with " + outcome + ": " +
                       command_text + "\n" + output);
  }
  if (log_failure) {
    std::rethrow_exception(log_failure);
  }
}

void CcmProvisioner::CleanupStaleCluster(const std::string &instance_id,
                                         int ccm_id,
                                         const fs::path &ccm_directory) {
  ValidateInstanceId(instance_id);
  ValidateCcmId(ccm_id);
  if (AbsoluteNormalized(ccm_directory) != clusters_directory_ / instance_id) {
    throw std::runtime_error("Refusing stale CCM configuration outside harness run: " +
                             AbsoluteNormalized(ccm_directory).string());
  }
  if (!fs::exists(ccm_directory)) {
    return;
  }
  CollectDiagnostics(instance_id, ccm_directory);
  RemoveByName(instance_id, ccm_directory);
}

} // namespace scylladb::alternator::testinfra
