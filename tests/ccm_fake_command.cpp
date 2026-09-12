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

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string Environment(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

void WriteAll(int fd, std::string_view contents) {
  while (!contents.empty()) {
    const auto written = ::write(fd, contents.data(), contents.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write failed: ") +
                               std::strerror(errno));
    }
    contents.remove_prefix(static_cast<std::size_t>(written));
  }
}

void AppendLine(const fs::path &path, const std::string &line) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error("cannot open observation file " + path.string());
  }
  try {
    WriteAll(fd, line + '\n');
  } catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) {
    throw std::runtime_error("cannot close observation file " + path.string());
  }
}

std::size_t FindArgument(const std::vector<std::string> &arguments,
                         const std::string &value) {
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == value) {
      return index;
    }
  }
  throw std::runtime_error("missing argument " + value);
}

fs::path ConfigDirectory(const std::vector<std::string> &arguments) {
  const auto index = FindArgument(arguments, "--config-dir");
  if (index + 1 >= arguments.size()) {
    throw std::runtime_error("--config-dir lacks value");
  }
  return arguments[index + 1];
}

std::string
ClusterNameForCreateOrRemove(const std::vector<std::string> &arguments) {
  const auto index = FindArgument(arguments, "--config-dir");
  if (index + 2 >= arguments.size()) {
    throw std::runtime_error("cluster command lacks name");
  }
  return arguments[index + 2];
}

void WriteText(const fs::path &path, const std::string &contents) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error("cannot open " + path.string());
  }
  try {
    WriteAll(fd, contents);
  } catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) {
    throw std::runtime_error("cannot close " + path.string());
  }
}

std::string ReadText(const fs::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::string contents;
  char buffer[4096];
  while (true) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      contents.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      const auto message = std::string("cannot read ") + path.string();
      ::close(fd);
      throw std::runtime_error(message);
    }
    break;
  }
  if (::close(fd) != 0) {
    throw std::runtime_error("cannot close " + path.string());
  }
  return contents;
}

void RemoveNodeMembership(const fs::path &cluster,
                          const std::string &node_name) {
  const auto cluster_config = cluster / "cluster.conf";
  const auto contents = ReadText(cluster_config);
  const auto node_entry = "- " + node_name;
  std::string updated;
  std::size_t begin = 0;
  while (begin < contents.size()) {
    const auto newline = contents.find('\n', begin);
    const auto end = newline == std::string::npos ? contents.size() : newline;
    if (contents.substr(begin, end - begin) != node_entry) {
      updated.append(contents, begin, end - begin);
      if (newline != std::string::npos) {
        updated.push_back('\n');
      }
    }
    if (newline == std::string::npos) {
      break;
    }
    begin = newline + 1;
  }
  WriteText(cluster_config, updated);
}

bool HasNodeMembership(const fs::path &cluster,
                       const std::string &node_name) {
  std::istringstream lines(ReadText(cluster / "cluster.conf"));
  std::string line;
  while (std::getline(lines, line)) {
    if (line == "- " + node_name) {
      return true;
    }
  }
  return false;
}

void AddNodeMembership(const fs::path &cluster,
                       const std::string &node_name) {
  const auto cluster_config = cluster / "cluster.conf";
  auto contents = ReadText(cluster_config);
  const auto seeds = contents.find("seeds:");
  if (seeds == std::string::npos) {
    throw std::runtime_error("cluster configuration lacks seeds");
  }
  contents.insert(seeds, "- " + node_name + "\n");
  WriteText(cluster_config, contents);
}

void CreateClusterState(const fs::path &config,
                        const std::string &cluster_name) {
  const auto cluster = config / cluster_name;
  fs::create_directories(cluster / "node1" / "conf");
  fs::create_directories(cluster / "node1" / "logs");
  WriteText(cluster / "cluster.conf",
            "name: " + cluster_name + "\nnodes:\n- node1\nseeds:\n- node1\n");
  WriteText(cluster / "node1" / "node.conf", "name: node1\nstatus: DOWN\n");
  WriteText(cluster / "node1" / "conf" / "scylla.yaml", "cluster_name: test\n");
  WriteText(config / "CURRENT", cluster_name + "\n");
}

void RemoveClusterState(const fs::path &config,
                        const std::string &cluster_name) {
  fs::remove_all(config / cluster_name);
  fs::remove(config / "CURRENT");
}

void SpawnTermIgnoringChild(const fs::path &observation_directory) {
  int ready[2];
  if (::pipe(ready) != 0) {
    throw std::runtime_error("pipe failed");
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(ready[0]);
    ::close(ready[1]);
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    ::close(ready[0]);
    std::signal(SIGTERM, SIG_IGN);
    const char marker = '1';
    (void)::write(ready[1], &marker, 1);
    ::close(ready[1]);
    while (true) {
      ::pause();
    }
  }

  ::close(ready[1]);
  char marker = 0;
  while (::read(ready[0], &marker, 1) < 0 && errno == EINTR) {
  }
  ::close(ready[0]);
  WriteText(observation_directory / "child.pid", std::to_string(child) + "\n");
}

int Run(const std::vector<std::string> &arguments) {
  if (arguments.empty()) {
    throw std::runtime_error("fake CCM requires command arguments");
  }
  const auto observation_directory =
      fs::path(Environment("ALTERNATOR_CLIENT_CPP_CCM_FAKE_OBSERVATIONS"));
  if (observation_directory.empty()) {
    throw std::runtime_error(
        "fake CCM observation directory is not configured");
  }
  fs::create_directories(observation_directory);
  AppendLine(observation_directory / "scylla-architectures.log",
             Environment("SCYLLA_ARCH"));

  std::string invocation;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) {
      invocation += ' ';
    }
    invocation += arguments[index];
  }
  AppendLine(observation_directory / "invocations.log", invocation);

  const auto mode = Environment("ALTERNATOR_CLIENT_CPP_CCM_FAKE_MODE");
  const auto config = ConfigDirectory(arguments);
  const auto &first = arguments.front();
  const auto second = arguments.size() > 1 ? arguments[1] : std::string{};

  if (first == "create") {
    const auto cluster_name = ClusterNameForCreateOrRemove(arguments);
    CreateClusterState(config, cluster_name);
    if (mode == "fail-create") {
      std::cout << "create failed after recording environment\n" << std::flush;
      return 47;
    }
    if (mode == "nonzero-create") {
      SpawnTermIgnoringChild(observation_directory);
      std::cout << "partial create output before nonzero exit\n" << std::flush;
      return 29;
    }
    if (mode == "timeout-create") {
      SpawnTermIgnoringChild(observation_directory);
      std::cout << "partial create output before timeout\n" << std::flush;
      while (true) {
        ::pause();
      }
    }
    if (mode == "diagnostic-failure") {
      std::cout << "partial create output before diagnostic failure\n"
                << std::flush;
      return 17;
    }
    return 0;
  }

  if (first == "remove") {
    if (mode != "record-only") {
      RemoveClusterState(config, ClusterNameForCreateOrRemove(arguments));
    }
    return 0;
  }

  if (first == "add") {
    const auto index = FindArgument(arguments, "--config-dir");
    if (index + 2 >= arguments.size()) {
      throw std::runtime_error("add command lacks node name");
    }
    const auto node_name = arguments[index + 2];
    std::string current = Environment("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER");
    if (current.empty()) {
      current = "cluster";
    }
    fs::create_directories(config / current / node_name / "conf");
    WriteText(config / current / node_name / "node.conf",
              "name: " + node_name + "\nstatus: DOWN\n");
    WriteText(config / current / node_name / "conf" / "scylla.yaml",
              "cluster_name: test\n");
    if (mode == "fail-add") {
      const auto link_target = observation_directory / "node-link-target";
      WriteText(link_target, "must survive partial-node cleanup\n");
      const auto link_directory =
          config / current / node_name / "bin" / "symlinks";
      fs::create_directories(link_directory);
      fs::create_symlink(link_target, link_directory / "scylla-jmx");
      std::cout << "partial add output\n" << std::flush;
      return 31;
    }
    AddNodeMembership(config / current, node_name);
    if (mode == "fail-add-member") {
      std::cout << "add failed after membership update\n" << std::flush;
      return 32;
    }
    return 0;
  }

  if (second == "remove") {
    std::string current = Environment("ALTERNATOR_CLIENT_CPP_CCM_FAKE_CLUSTER");
    if (current.empty()) {
      current = "cluster";
    }
    const auto cluster = config / current;
    if (!HasNodeMembership(cluster, first)) {
      std::cout << "unknown node " << first << "\n" << std::flush;
      return 44;
    }
    if (mode != "partial-node-remove") {
      RemoveNodeMembership(cluster, first);
    }
    fs::remove_all(cluster / first);
    if (mode == "partial-node-remove") {
      std::cout << "node directory removed before membership update\n"
                << std::flush;
      return 43;
    }
    return 0;
  }

  if (second == "start" && mode == "fail-node-start") {
    std::cout << "node start failed after topology update\n" << std::flush;
    return 41;
  }

  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (Environment("ALTERNATOR_CLIENT_CPP_CCM_FAKE_PROCESS_MODE") ==
        "stay-alive") {
      while (true) {
        ::pause();
      }
    }
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    return Run(arguments);
  } catch (const std::exception &failure) {
    std::cerr << "fake CCM failed: " << failure.what() << '\n';
    return 111;
  }
}
