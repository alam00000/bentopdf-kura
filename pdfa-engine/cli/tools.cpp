#include "tools.hh"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kuratools {
namespace {
namespace fs = std::filesystem;

std::mutex& spawnMutex() {
  static std::mutex m;
  return m;
}

#ifdef _WIN32
std::string winQuote(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string o = "\"";
  size_t bs = 0;
  for (char c : a) {
    if (c == '\\') {
      ++bs;
      continue;
    }
    o.append(c == '"' ? bs * 2 + 1 : bs, '\\');
    bs = 0;
    o += c;
  }
  o.append(bs * 2, '\\');
  return o + "\"";
}
#else
long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
}

#ifdef _WIN32
int runProcess(const std::vector<std::string>& argv, int timeoutSec, std::string* out) {
  if (argv.empty()) return kSpawnFail;
  std::string cmd;
  for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + winQuote(a);
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  PROCESS_INFORMATION pi{};
  HANDLE rd = nullptr;
  BOOL started = FALSE;
  {
    std::lock_guard<std::mutex> lock(spawnMutex());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE wr = nullptr, err = nullptr;
    if (out && CreatePipe(&rd, &wr, &sa, 0)) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE se = GetStdHandle(STD_ERROR_HANDLE);
    if (out && se && se != INVALID_HANDLE_VALUE)
      DuplicateHandle(GetCurrentProcess(), se, GetCurrentProcess(), &err, 0, TRUE, DUPLICATE_SAME_ACCESS);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out ? wr : nul;
    si.hStdError = err ? err : nul;
    if (nul != INVALID_HANDLE_VALUE && (!out || wr))
      started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | (out ? 0 : CREATE_NO_WINDOW),
                               nullptr, nullptr, &si, &pi);
    for (HANDLE h : {nul, wr, err})
      if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  if (!started) {
    if (rd) CloseHandle(rd);
    if (job) CloseHandle(job);
    return kSpawnFail;
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  if (rd) {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out->append(buf, n);
    CloseHandle(rd);
  }
  int rc;
  if (WaitForSingleObject(pi.hProcess, timeoutSec > 0 ? DWORD(timeoutSec) * 1000 : INFINITE) == WAIT_TIMEOUT) {
    if (!job || !TerminateJobObject(job, 1)) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    rc = kTimedOut;
  } else {
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    rc = int(code);
  }
  if (job) CloseHandle(job);
  CloseHandle(pi.hProcess);
  return rc;
}
#else
int runProcess(const std::vector<std::string>& argv, int timeoutSec, std::string* out) {
  if (argv.empty()) return kSpawnFail;
  std::vector<std::string> copy = argv;
  std::vector<char*> av;
  for (auto& a : copy) av.push_back(a.data());
  av.push_back(nullptr);
  pid_t pid = 0;
  int fds[2] = {-1, -1}, rc;
  {
    std::lock_guard<std::mutex> lock(spawnMutex());
    if (out && pipe(fds) != 0) return kSpawnFail;
    if (out) fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) {
      posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
      posix_spawn_file_actions_addclose(&fa, fds[1]);
    } else {
      posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    if (!out) {
      posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
      posix_spawnattr_setpgroup(&at, 0);
    }
    rc = posix_spawnp(&pid, av[0], &fa, &at, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (out) close(fds[1]);
  }
  if (rc != 0) {
    if (out) close(fds[0]);
    return kSpawnFail;
  }
  if (out) {
    char buf[4096];
    for (;;) {
      ssize_t n = read(fds[0], buf, sizeof buf);
      if (n > 0) out->append(buf, size_t(n));
      else if (n == 0 || errno != EINTR) break;
    }
    close(fds[0]);
  }
  long long end = timeoutSec > 0 ? nowMs() + 1000LL * timeoutSec : 0;
  for (;;) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) {
      if (!out) kill(-pid, SIGKILL);
      return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    }
    if (w < 0 && errno != EINTR) return kSpawnFail;
    if (end && nowMs() > end) {
      kill(out ? pid : -pid, SIGKILL);
      waitpid(pid, &st, 0);
      return kTimedOut;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(out ? 5 : 50));
  }
}
#endif

std::string findProgram(const char* env, std::vector<std::string> names, const std::vector<std::string>& paths) {
  std::error_code ec;
  if (const char* e = std::getenv(env); e && *e) return e;
#ifdef _WIN32
  const char sep = ';';
  for (auto& n : names) n += ".exe";
#else
  const char sep = ':';
#endif
  if (const char* p = std::getenv("PATH")) {
    std::string path = p;
    for (size_t a = 0; a <= path.size();) {
      size_t b = path.find(sep, a);
      if (b == std::string::npos) b = path.size();
      std::string dir = path.substr(a, b - a);
      a = b + 1;
      if (dir.size() > 1 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
      if (dir.empty()) continue;
      for (auto& n : names)
        if (fs::is_regular_file(fs::path(dir) / n, ec)) return (fs::path(dir) / n).string();
    }
  }
  for (auto& p : paths)
    if (fs::is_regular_file(p, ec)) return p;
  return "";
}

int runTool(int argc, char** argv) {
  if (argc < 1) {
    std::cerr << "usage: kura tool <name> [args...]" << std::endl;
    return 64;
  }
  std::string name = argv[0];
  std::vector<std::string> args(argv + 1, argv + argc);
  for (auto fn : {toolOutput, toolReport}) {
    int rc = fn(name, args);
    if (rc != kUnknownTool) return rc;
  }
  std::cerr << "unknown tool: " << name << std::endl;
  return 64;
}
}
