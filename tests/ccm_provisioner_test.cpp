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

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace scylladb::alternator::testinfra;

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(const std::string &stem) {
    auto pattern = (fs::temp_directory_path() / (stem + ".XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char *created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const fs::path &Path() const noexcept { return path_; }

private:
  fs::path path_;
};

class ScopedEnvironment final {
public:
  ScopedEnvironment(std::string name, const std::string &value)
      : name_(std::move(name)) {
    const char *previous = std::getenv(name_.c_str());
    if (previous != nullptr) {
      previous_ = previous;
    }
    if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("setenv failed for " + name_);
    }
  }

  ~ScopedEnvironment() {
    if (previous_) {
      (void)::setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

class ChildProcessGuard final {
public:
  explicit ChildProcessGuard(pid_t pid) : pid_(pid) {}

  ~ChildProcessGuard() {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGKILL);
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }

  ChildProcessGuard(const ChildProcessGuard &) = delete;
  ChildProcessGuard &operator=(const ChildProcessGuard &) = delete;

  int Wait() {
    int status = 0;
    pid_t result;
    do {
      result = ::waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != pid_) {
      throw std::runtime_error("waitpid failed");
    }
    pid_ = -1;
    return status;
  }

private:
  pid_t pid_;
};

class ScopedChildSubreaper final {
public:
  ScopedChildSubreaper() {
    if (::prctl(PR_GET_CHILD_SUBREAPER, &previous_) != 0 ||
        ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
      throw std::runtime_error("cannot enable child subreaper");
    }
  }

  ~ScopedChildSubreaper() {
    (void)::prctl(PR_SET_CHILD_SUBREAPER, previous_);
  }

  ScopedChildSubreaper(const ScopedChildSubreaper &) = delete;
  ScopedChildSubreaper &operator=(const ScopedChildSubreaper &) = delete;

private:
  int previous_ = 0;
};

fs::path FakeCommandPath() {
  const char *configured =
      std::getenv("ALTERNATOR_CLIENT_CPP_CCM_FAKE_COMMAND");
  if (configured != nullptr && *configured != '\0') {
    return fs::absolute(configured);
  }
  return fs::canonical("/proc/self/exe").parent_path() /
         "alternator_client_cpp_ccm_fake_command";
}

std::string ReadFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void WriteFile(const fs::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write " + path.string());
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

ClusterSpec OneNodeHttpSpec() {
  return ClusterSpec{}
      .WithTopology(ClusterTopology::SingleDatacenter(1))
      .WithTransports({AlternatorTransport::Http});
}

std::string ExpectedScyllaArchitecture() {
  struct utsname host {};
  if (::uname(&host) != 0) {
    throw std::runtime_error("uname failed");
  }
  const std::string machine(host.machine);
  if (machine == "x86_64" || machine == "amd64") {
    return "x86_64";
  }
  if (machine == "aarch64" || machine == "arm64") {
    return "aarch64";
  }
  throw std::runtime_error("unsupported test host architecture: " + machine);
}

fs::path CreateDirectory(const fs::path &path) {
  fs::create_directories(path);
  return path;
}

struct HarnessPaths {
  explicit HarnessPaths(const std::string &stem)
      : temporary(stem), run(CreateDirectory(temporary.Path() / "run")),
        diagnostics(CreateDirectory(temporary.Path() / "diagnostics")),
        observations(CreateDirectory(temporary.Path() / "observations")) {}

  TemporaryDirectory temporary;
  fs::path run;
  fs::path diagnostics;
  fs::path observations;
};

class FakeMode final {
public:
  FakeMode(const fs::path &observations, const std::string &mode)
      : executable_("ALTERNATOR_CLIENT_CPP_CCM_FAKE_COMMAND",
                    FakeCommandPath().string()),
        observations_("ALTERNATOR_CLIENT_CPP_CCM_FAKE_OBSERVATIONS",
                      observations.string()),
        mode_("ALTERNATOR_CLIENT_CPP_CCM_FAKE_MODE", mode) {
    if (!fs::is_regular_file(FakeCommandPath())) {
      throw std::runtime_error("CCM fake command is unavailable at " +
                               FakeCommandPath().string());
    }
  }

private:
  ScopedEnvironment executable_;
  ScopedEnvironment observations_;
  ScopedEnvironment mode_;
};

bool ProcessAlive(pid_t pid) {
  if (::kill(pid, 0) != 0 && errno != EPERM) {
    return false;
  }
  try {
    const auto stat =
        ReadFile(fs::path("/proc") / std::to_string(pid) / "stat");
    const auto end = stat.rfind(')');
    return end == std::string::npos || end + 2 >= stat.size() ||
           stat[end + 2] != 'Z';
  } catch (...) {
    return false;
  }
}

pid_t ReadPid(const fs::path &path) {
  return static_cast<pid_t>(std::stol(ReadFile(path)));
}

void ExpectProcessGone(pid_t pid) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (ProcessAlive(pid) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(ProcessAlive(pid))
      << "fake CCM descendant remains alive: " << pid;
}

void WaitForProcessCommand(pid_t pid, const fs::path &expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const auto command =
          ReadFile(fs::path("/proc") / std::to_string(pid) / "cmdline");
      if (command.substr(0, command.find('\0')) == expected.string()) {
        return;
      }
    } catch (...) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  throw std::runtime_error("fake node process did not exec expected command");
}

std::vector<fs::path> CommandLogs(const fs::path &ccm_directory) {
  std::vector<fs::path> result;
  for (const auto &entry : fs::directory_iterator(ccm_directory)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() && name.rfind("ccm-command-", 0) == 0 &&
        entry.path().extension() == ".log") {
      result.push_back(entry.path());
    }
  }
  return result;
}

std::string ConcatenateFiles(const std::vector<fs::path> &paths) {
  std::string result;
  for (const auto &path : paths) {
    result += ReadFile(path);
  }
  return result;
}

void CreateMinimalCluster(const fs::path &ccm_directory,
                          const std::string &instance_id,
                          bool with_diagnostics) {
  const auto cluster = ccm_directory / instance_id;
  fs::create_directories(cluster / "node1" / "conf");
  fs::create_directories(cluster / "node1" / "logs");
  WriteFile(cluster / "cluster.conf",
            "name: " + instance_id + "\nnodes:\n- node1\nseeds:\n- node1\n");
  WriteFile(cluster / "node1" / "node.conf", "name: node1\nstatus: DOWN\n");
  WriteFile(cluster / "node1" / "conf" / "scylla.yaml", "cluster_name: test\n");
  WriteFile(ccm_directory / "CURRENT", instance_id + "\n");
  if (with_diagnostics) {
    WriteFile(cluster / "node1" / "logs" / "system.log", "useful diagnostic\n");
    WriteFile(cluster / "node1" / "logs" / "server.key", "private key\n");
    WriteFile(cluster / "node1" / "logs" / "client.pem", "private pem\n");
    WriteFile(ccm_directory / "ccm-command-before-crash.log",
              "partial command\n");
  }
}

void AddSecondNodeToMinimalCluster(const fs::path &ccm_directory,
                                   const std::string &instance_id) {
  const auto cluster = ccm_directory / instance_id;
  fs::create_directories(cluster / "node2" / "conf");
  fs::create_directories(cluster / "node2" / "logs");
  WriteFile(cluster / "node2" / "node.conf",
            "name: node2\nstatus: DOWN\n");
  WriteFile(cluster / "node2" / "conf" / "scylla.yaml",
            "cluster_name: test\n");
  WriteFile(cluster / "cluster.conf",
            "name: " + instance_id +
                "\nnodes:\n- node1\n- node2\nseeds:\n- node1\n");
}

TEST(CcmProvisionerTest, NonzeroCommandReapsProcessGroupAndKeepsDurableLogs) {
  HarnessPaths paths("ccm-provisioner-nonzero");
  ScopedChildSubreaper subreaper;
  FakeMode mode(paths.observations, "nonzero-create");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string(),
                             std::chrono::seconds(10));

  try {
    (void)provisioner.Provision(OneNodeHttpSpec(), "nonzero-cluster", 71);
    FAIL() << "provisioning unexpectedly succeeded";
  } catch (const ClusterProvisioningError &failure) {
    EXPECT_TRUE(failure.CleanupProven());
    EXPECT_NE(std::string(failure.what()).find("rollback succeeded"),
              std::string::npos);
  }

  const auto ccm_directory = paths.run / "clusters" / "nonzero-cluster";
  ASSERT_TRUE(fs::is_regular_file(paths.observations / "child.pid"));
  const auto child_pid = ReadPid(paths.observations / "child.pid");
  ChildProcessGuard child(child_pid);
  ExpectProcessGone(child_pid);
  const int child_status = child.Wait();
  ASSERT_TRUE(WIFSIGNALED(child_status));
  EXPECT_EQ(WTERMSIG(child_status), SIGKILL);
  const auto logs = ConcatenateFiles(CommandLogs(ccm_directory));
  EXPECT_NE(logs.find("partial create output before nonzero exit"),
            std::string::npos);
  EXPECT_NE(logs.find("[exit 29]"), std::string::npos);
  EXPECT_TRUE(fs::is_regular_file(ccm_directory / "ccm-commands.log"));
  EXPECT_FALSE(fs::exists(ccm_directory / "nonzero-cluster"));
}

TEST(CcmProvisionerTest, CcmCommandsOverrideInheritedScyllaArchitecture) {
  HarnessPaths paths("ccm-provisioner-architecture");
  ScopedEnvironment inherited_architecture("SCYLLA_ARCH", "incorrect");
  FakeMode mode(paths.observations, "fail-create");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());

  EXPECT_THROW(
      (void)provisioner.Provision(OneNodeHttpSpec(), "architecture-cluster", 83),
      ClusterProvisioningError);

  std::istringstream architectures(
      ReadFile(paths.observations / "scylla-architectures.log"));
  std::string architecture;
  std::size_t invocation_count = 0;
  while (std::getline(architectures, architecture)) {
    EXPECT_EQ(architecture, ExpectedScyllaArchitecture());
    ++invocation_count;
  }
  EXPECT_GE(invocation_count, 2U);
}

TEST(CcmProvisionerTest, TimedOutCommandReapsProcessGroupAndKeepsDurableLogs) {
  HarnessPaths paths("ccm-provisioner-timeout");
  FakeMode mode(paths.observations, "timeout-create");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string(),
                             std::chrono::seconds(1));

  try {
    (void)provisioner.Provision(OneNodeHttpSpec(), "timeout-cluster", 72);
    FAIL() << "provisioning unexpectedly succeeded";
  } catch (const ClusterProvisioningError &failure) {
    EXPECT_TRUE(failure.CleanupProven());
    EXPECT_NE(std::string(failure.what()).find("rollback succeeded"),
              std::string::npos);
  }

  const auto ccm_directory = paths.run / "clusters" / "timeout-cluster";
  ASSERT_TRUE(fs::is_regular_file(paths.observations / "child.pid"));
  ExpectProcessGone(ReadPid(paths.observations / "child.pid"));
  const auto logs = ConcatenateFiles(CommandLogs(ccm_directory));
  EXPECT_NE(logs.find("partial create output before timeout"),
            std::string::npos);
  EXPECT_NE(logs.find("[exit timeout]"), std::string::npos);
  EXPECT_FALSE(fs::exists(ccm_directory / "timeout-cluster"));
}

TEST(CcmProvisionerTest, MalformedMetadataIsQuarantinedBeforeCcmRuns) {
  HarnessPaths paths("ccm-provisioner-malformed");
  FakeMode mode(paths.observations, "record-only");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "malformed-cluster";
  fs::create_directories(ccm_directory / "malformed-cluster");
  WriteFile(ccm_directory / "malformed-cluster" / "cluster.conf",
            "name: [unterminated\n");
  WriteFile(ccm_directory / "CURRENT", "malformed-cluster\n");

  EXPECT_THROW(
      provisioner.CleanupStaleCluster("malformed-cluster", 73, ccm_directory),
      std::exception);
  EXPECT_FALSE(fs::exists(paths.observations / "invocations.log"));
  EXPECT_TRUE(fs::is_regular_file(ccm_directory / "malformed-cluster" /
                                  "cluster.conf"));
}

TEST(CcmProvisionerTest, DiagnosticFailurePreservesFailedClusterState) {
  HarnessPaths paths("ccm-provisioner-diagnostic-failure");
  FakeMode mode(paths.observations, "diagnostic-failure");
  const std::string instance_id = "diagnostic-cluster";
  WriteFile(paths.diagnostics / instance_id, "blocks diagnostic directory\n");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());

  try {
    (void)provisioner.Provision(OneNodeHttpSpec(), instance_id, 74);
    FAIL() << "provisioning unexpectedly succeeded";
  } catch (const ClusterProvisioningError &failure) {
    EXPECT_FALSE(failure.CleanupProven());
    EXPECT_NE(std::string(failure.what()).find("diagnostic snapshot failed"),
              std::string::npos);
  }

  const auto ccm_directory = paths.run / "clusters" / instance_id;
  EXPECT_TRUE(
      fs::is_regular_file(ccm_directory / instance_id / "cluster.conf"));
  const auto invocations = ReadFile(paths.observations / "invocations.log");
  EXPECT_NE(invocations.find("create --config-dir"), std::string::npos);
  EXPECT_EQ(invocations.find("remove --config-dir"), std::string::npos);
}

TEST(CcmProvisionerTest, FailedNodeAddRollsBackExactlyOnce) {
  HarnessPaths paths("ccm-provisioner-add-rollback");
  FakeMode mode(paths.observations, "fail-add-member");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  ProvisionedClusterData data;
  data.ccm_id = 75;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(TestClusterNode{"node1", "127.0.75.1", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      std::make_shared<CcmProvisioner>(paths.run, paths.diagnostics,
                                       FakeCommandPath().string()),
      "cluster", OneNodeHttpSpec(), std::move(data));

  try {
    (void)provisioner.AddNode(cluster, "dc1", "RAC1");
    FAIL() << "node addition unexpectedly succeeded";
  } catch (const NodeProvisioningError &failure) {
    EXPECT_TRUE(failure.CleanupProven());
    EXPECT_NE(std::string(failure.what()).find("rollback succeeded"),
              std::string::npos);
  }

  EXPECT_FALSE(fs::exists(ccm_directory / "cluster" / "node2"));
  const auto invocations = ReadFile(paths.observations / "invocations.log");
  EXPECT_EQ(invocations.find("add --config-dir"), 0U);
  const auto rollback = invocations.find("node2 remove --config-dir");
  ASSERT_NE(rollback, std::string::npos);
  EXPECT_EQ(invocations.find("node2 remove --config-dir", rollback + 1),
            std::string::npos);
}

TEST(CcmProvisionerTest,
     FailedNonmemberNodeAddSafelyRemovesPartialDirectory) {
  HarnessPaths paths("ccm-provisioner-partial-add-rollback");
  FakeMode mode(paths.observations, "fail-add");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  const auto provisioner = std::make_shared<CcmProvisioner>(
      paths.run, paths.diagnostics, FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  ProvisionedClusterData data;
  data.ccm_id = 84;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.84.1", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      provisioner, "cluster", OneNodeHttpSpec(), std::move(data));

  try {
    (void)cluster.AddNode("dc1", "RAC1");
    FAIL() << "node addition unexpectedly succeeded";
  } catch (const NodeProvisioningError &failure) {
    EXPECT_TRUE(failure.CleanupProven());
    EXPECT_FALSE(failure.RecoveryRequired());
    EXPECT_NE(std::string(failure.what()).find("rollback succeeded"),
              std::string::npos);
  }

  EXPECT_FALSE(cluster.IsDirty());
  EXPECT_FALSE(fs::exists(ccm_directory / "cluster" / "node2"));
  EXPECT_EQ(ReadFile(paths.observations / "node-link-target"),
            "must survive partial-node cleanup\n");
  EXPECT_EQ(ReadFile(ccm_directory / "cluster" / "cluster.conf")
                .find("- node2"),
            std::string::npos);
  const auto invocations = ReadFile(paths.observations / "invocations.log");
  EXPECT_EQ(invocations.find("add --config-dir"), 0U);
  EXPECT_EQ(invocations.find("node2 remove --config-dir"), std::string::npos);
  EXPECT_NO_THROW(cluster.Stop());
}

TEST(CcmProvisionerTest,
     FailedNodeStartRollsBackButKeepsTopologyStateAmbiguous) {
  HarnessPaths paths("ccm-provisioner-start-rollback");
  FakeMode mode(paths.observations, "fail-node-start");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  ProvisionedClusterData data;
  data.ccm_id = 77;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.77.1", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      std::make_shared<CcmProvisioner>(paths.run, paths.diagnostics,
                                       FakeCommandPath().string()),
      "cluster", OneNodeHttpSpec(), std::move(data));

  try {
    (void)provisioner.AddNode(cluster, "dc1", "RAC1");
    FAIL() << "node addition unexpectedly succeeded";
  } catch (const NodeProvisioningError &failure) {
    EXPECT_FALSE(failure.CleanupProven());
    EXPECT_FALSE(failure.RecoveryRequired());
    EXPECT_NE(std::string(failure.what()).find("rollback succeeded"),
              std::string::npos);
  }

  EXPECT_FALSE(fs::exists(ccm_directory / "cluster" / "node2"));
  const auto invocations = ReadFile(paths.observations / "invocations.log");
  EXPECT_NE(invocations.find("node2 start --config-dir"), std::string::npos);
  EXPECT_NE(invocations.find("node2 remove --config-dir"), std::string::npos);
}

TEST(CcmProvisionerTest, MappingRootAndDottedOverrideMergeOnAddedNode) {
  HarnessPaths paths("ccm-provisioner-mapping-merge");
  FakeMode mode(paths.observations, "fail-node-start");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  const auto spec = OneNodeHttpSpec()
                        .WithYamlOverride("custom_mapping", "{existing: true}")
                        .WithYamlOverride("custom_mapping.child", "42")
                        .WithYamlOverride("quoted_scalar", "'true'");
  ProvisionedClusterData data;
  data.ccm_id = 79;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.79.1", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      std::make_shared<CcmProvisioner>(paths.run, paths.diagnostics,
                                       FakeCommandPath().string()),
      "cluster", spec, std::move(data));

  EXPECT_THROW(provisioner.AddNode(cluster, "dc1", "RAC1"),
               NodeProvisioningError);

  const auto snapshot = paths.diagnostics / "cluster" / "cluster" / "node2" /
                        "conf" / "scylla.yaml";
  ASSERT_TRUE(fs::is_regular_file(snapshot));
  const auto yaml = YAML::Load(ReadFile(snapshot));
  ASSERT_TRUE(yaml["custom_mapping"].IsMap());
  EXPECT_TRUE(yaml["custom_mapping"]["existing"].as<bool>());
  EXPECT_EQ(yaml["custom_mapping"]["child"].as<int>(), 42);
  EXPECT_EQ(yaml["quoted_scalar"].as<std::string>(), "true");
  EXPECT_EQ(yaml["quoted_scalar"].Tag(), "tag:yaml.org,2002:str");
}

TEST(CcmProvisionerTest, NodeRemovalClearsDirectoryAndClusterMembership) {
  HarnessPaths paths("ccm-provisioner-node-remove");
  FakeMode mode(paths.observations, "remove-success");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  const auto provisioner = std::make_shared<CcmProvisioner>(
      paths.run, paths.diagnostics, FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  AddSecondNodeToMinimalCluster(ccm_directory, "cluster");
  ProvisionedClusterData data;
  data.ccm_id = 80;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.80.1", "dc1", "RAC1"});
  data.nodes.push_back(
      TestClusterNode{"node2", "127.0.80.2", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      provisioner, "cluster",
      OneNodeHttpSpec().WithTopology(ClusterTopology::SingleDatacenter(2)),
      std::move(data));

  const auto removed = cluster.Nodes().front();
  EXPECT_NO_THROW(cluster.RemoveNode(removed));

  ASSERT_EQ(cluster.Nodes().size(), 1U);
  EXPECT_EQ(cluster.Nodes().front().name, "node2");
  EXPECT_FALSE(cluster.IsDirty());
  EXPECT_FALSE(fs::exists(ccm_directory / "cluster" / "node1"));
  const auto cluster_config =
      ReadFile(ccm_directory / "cluster" / "cluster.conf");
  EXPECT_EQ(cluster_config.find("- node1"), std::string::npos);
  EXPECT_NE(cluster_config.find("- node2"), std::string::npos);
}

TEST(CcmProvisionerTest,
     PartialNodeRemovalRetainsLogicalNodeAndPoisonsCluster) {
  HarnessPaths paths("ccm-provisioner-partial-node-remove");
  FakeMode mode(paths.observations, "partial-node-remove");
  ScopedEnvironment cluster_name("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER",
                                 "cluster");
  const auto provisioner = std::make_shared<CcmProvisioner>(
      paths.run, paths.diagnostics, FakeCommandPath().string());
  const auto ccm_directory = paths.run / "clusters" / "config";
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, "cluster", false);
  AddSecondNodeToMinimalCluster(ccm_directory, "cluster");
  ProvisionedClusterData data;
  data.ccm_id = 81;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.81.1", "dc1", "RAC1"});
  data.nodes.push_back(
      TestClusterNode{"node2", "127.0.81.2", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      provisioner, "cluster",
      OneNodeHttpSpec().WithTopology(ClusterTopology::SingleDatacenter(2)),
      std::move(data));

  const auto removed = cluster.Nodes().front();
  try {
    cluster.RemoveNode(removed);
    FAIL() << "partial node removal unexpectedly succeeded";
  } catch (const std::runtime_error &failure) {
    EXPECT_NE(std::string(failure.what()).find("exit 43"), std::string::npos);
  }

  EXPECT_EQ(cluster.Nodes().size(), 2U);
  EXPECT_TRUE(cluster.IsDirty());
  EXPECT_FALSE(fs::exists(ccm_directory / "cluster" / "node1"));
  EXPECT_NE(ReadFile(ccm_directory / "cluster" / "cluster.conf")
                .find("- node1"),
            std::string::npos);
  EXPECT_THROW(cluster.Start(), std::logic_error);
}

TEST(CcmProvisionerTest,
     ClusterRemovalRejectsCapturedProcessAfterCcmDeletesPidMetadata) {
  HarnessPaths paths("ccm-provisioner-captured-process");
  FakeMode mode(paths.observations, "remove-success");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const std::string instance_id = "captured-process-cluster";
  const auto ccm_directory = paths.run / "clusters" / instance_id;
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, instance_id, true);

  const auto node_directory = ccm_directory / instance_id / "node1";
  const auto fake_scylla = node_directory / "bin" / "scylla";
  fs::create_directories(fake_scylla.parent_path());
  fs::copy_file(FakeCommandPath(), fake_scylla);
  fs::permissions(fake_scylla,
                  fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::owner_exec,
                  fs::perm_options::replace);

  const pid_t child = ::fork();
  if (child == 0) {
    (void)::setenv("SCYLLA_CCM_RUN_DIR", paths.run.c_str(), 1);
    (void)::setenv("ALTERNATOR_CLIENT_CPP_CCM_FAKE_PROCESS_MODE",
                   "stay-alive", 1);
    ::execl(fake_scylla.c_str(), fake_scylla.c_str(),
            static_cast<char *>(nullptr));
    _exit(127);
  }
  ASSERT_GT(child, 0);
  ChildProcessGuard child_guard(child);
  WaitForProcessCommand(child, fake_scylla);
  WriteFile(node_directory / "node.conf",
            "name: node1\nstatus: UP\npid: " + std::to_string(child) +
                "\n");
  WriteFile(node_directory / "cassandra.pid", std::to_string(child) + "\n");

  ProvisionedClusterData data;
  data.ccm_id = 78;
  data.ccm_directory = ccm_directory;
  data.nodes.push_back(
      TestClusterNode{"node1", "127.0.78.1", "dc1", "RAC1"});
  PhysicalTestCluster cluster(
      std::make_shared<CcmProvisioner>(paths.run, paths.diagnostics,
                                       FakeCommandPath().string()),
      instance_id, OneNodeHttpSpec(), std::move(data));

  EXPECT_THROW(provisioner.Remove(cluster), RecoveryRequiredError);
  EXPECT_TRUE(ProcessAlive(child));
  EXPECT_FALSE(fs::exists(ccm_directory / instance_id));
}

TEST(CcmProvisionerTest, DiagnosticsExcludePrivateKeys) {
  HarnessPaths paths("ccm-provisioner-diagnostics");
  FakeMode mode(paths.observations, "remove-success");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const std::string instance_id = "diagnostic-copy-cluster";
  const auto ccm_directory = paths.run / "clusters" / instance_id;
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, instance_id, true);

  provisioner.CleanupStaleCluster(instance_id, 76, ccm_directory);

  const auto destination = paths.diagnostics / instance_id;
  EXPECT_EQ(
      ReadFile(destination / instance_id / "node1" / "logs" / "system.log"),
      "useful diagnostic\n");
  EXPECT_EQ(ReadFile(destination / "ccm-command-before-crash.log"),
            "partial command\n");
  EXPECT_FALSE(
      fs::exists(destination / instance_id / "node1" / "logs" / "server.key"));
  EXPECT_FALSE(
      fs::exists(destination / instance_id / "node1" / "logs" / "client.pem"));
  EXPECT_FALSE(fs::exists(ccm_directory / instance_id));
}

TEST(CcmProvisionerTest,
     DiagnosticsRejectSymlinkedNodeConfigurationDirectory) {
  HarnessPaths paths("ccm-provisioner-symlinked-node-conf");
  FakeMode mode(paths.observations, "remove-success");
  CcmProvisioner provisioner(paths.run, paths.diagnostics,
                             FakeCommandPath().string());
  const std::string instance_id = "symlinked-node-conf-cluster";
  const auto ccm_directory = paths.run / "clusters" / instance_id;
  fs::create_directory(ccm_directory);
  CreateMinimalCluster(ccm_directory, instance_id, false);

  const auto outside = paths.temporary.Path() / "outside-node-conf";
  fs::create_directory(outside);
  WriteFile(outside / "scylla.yaml", "must_not_be_copied: true\n");
  const auto node_configuration =
      ccm_directory / instance_id / "node1" / "conf";
  fs::remove_all(node_configuration);
  fs::create_directory_symlink(outside, node_configuration);

  EXPECT_THROW(
      provisioner.CleanupStaleCluster(instance_id, 82, ccm_directory),
      std::runtime_error);

  EXPECT_TRUE(fs::is_symlink(fs::symlink_status(node_configuration)));
  EXPECT_TRUE(fs::is_regular_file(outside / "scylla.yaml"));
  EXPECT_TRUE(fs::exists(ccm_directory / instance_id));
  EXPECT_FALSE(fs::exists(paths.diagnostics / instance_id / instance_id /
                          "node1" / "conf" / "scylla.yaml"));
  EXPECT_FALSE(fs::exists(paths.observations / "invocations.log"));
}

} // namespace
