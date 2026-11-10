#include "semphr.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#include "../SimLog.h"

namespace {

// One host mutex guards every semaphore, every mutex holder and every task's
// waitingOn. Lock traffic in the firmware is light, and a single lock makes
// the deadlock walk below consistent.
std::mutex &stateMutex() {
  // Firmware global constructors create semaphores before this file's
  // globals would be initialized.
  static std::mutex m;
  return m;
}

// Long mutex waits get one report per take.
constexpr auto kLongWait = std::chrono::seconds(5);

std::chrono::milliseconds ticksToDuration(uint32_t ticks) {
  return std::chrono::milliseconds(static_cast<int64_t>(ticks) *
                                   portTICK_PERIOD_MS);
}

const char *taskName(TaskHandle_t task) {
  return task && task->name ? task->name : "?";
}

void logLine(const char *format, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char *format, ...) {
  char line[320];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line) - 1, format, args);
  va_end(args);
  size_t len = n < 0 ? 0 : std::min<size_t>(n, sizeof(line) - 2);
  line[len++] = '\n';
  fwrite(line, 1, len, stderr);
  sim_log::write(line, len);
}

// Prints one backtrace to stderr. On Linux, frames in the simulator binary go
// through addr2line for function names and source lines; otherwise (or if
// addr2line is missing) the libc symbol strings are printed.
void printTrace(void *const *frames, int count) {
#ifdef __linux__
  std::string offsets;
  std::string exe;
  for (int i = 0; i < count; ++i) {
    Dl_info info;
    if (!dladdr(frames[i], &info) || !info.dli_fname)
      continue;
    char exePath[512];
    const ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0)
      break;
    exePath[n] = '\0';
    char realName[512];
    if (!realpath(info.dli_fname, realName) || strcmp(realName, exePath) != 0)
      continue;
    exe = exePath;
    char buf[32];
    // -1: the return address points after the call.
    snprintf(buf, sizeof(buf), " 0x%zx",
             static_cast<size_t>(static_cast<char *>(frames[i]) -
                                 static_cast<char *>(info.dli_fbase) - 1));
    offsets += buf;
  }
  if (!exe.empty()) {
    const std::string cmd = "addr2line -Cfpie '" + exe + "'" + offsets;
    if (FILE *p = popen(cmd.c_str(), "r")) {
      char line[512];
      bool any = false;
      while (fgets(line, sizeof(line), p)) {
        if (strncmp(line, "?? ", 3) == 0)
          continue;
        fprintf(stderr, "    %s", line);
        any = true;
      }
      if (pclose(p) == 0 && any)
        return;
    }
  }
#endif
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

void printCurrentTrace() {
  void *frames[SimMutex::kTraceDepth];
  const int count = backtrace(frames, SimMutex::kTraceDepth);
  // Skip this function and the report that called it.
  printTrace(frames + 2, count > 2 ? count - 2 : 0);
}

void printHolderTrace(const SimMutex &m) {
  fprintf(stderr, "  \"%s\" took the mutex at:\n", taskName(m.holder));
  printTrace(m.takenAt, m.takenAtFrames);
}

[[noreturn]] void failAssert(const char *what, SemaphoreHandle_t sem) {
  logLine("[SIM-FREERTOS] ASSERT: %s (handle %p, task \"%s\"). FreeRTOS "
          "configASSERT aborts here on the device.",
          what, static_cast<void *>(sem),
          taskName(xTaskGetCurrentTaskHandle()));
  printCurrentTrace();
  fflush(stderr);
  abort();
}

void acquire(SimMutex &m, TaskHandle_t self) {
  m.holder = self;
  m.depth = 1;
  m.takenAtFrames = backtrace(m.takenAt, SimMutex::kTraceDepth);
}

// The task at the end of the chain "m's holder waits for a mutex whose holder
// waits for ...". Returns true when the chain comes back to `self`.
bool waitsForSelf(const SimMutex &m, TaskHandle_t self) {
  TaskHandle_t task = m.holder;
  for (int hops = 0; task && hops < 32; ++hops) {
    if (task == self)
      return true;
    auto *next = static_cast<SimSemaphoreBase *>(task->waitingOn);
    if (!next || next->kind == SimSemaphoreKind::Counting)
      return false;
    task = static_cast<SimMutex *>(next)->holder;
  }
  return false;
}

void reportDeadlock(const SimMutex &m, TaskHandle_t self, uint32_t ticks) {
  const bool forever = ticks == portMAX_DELAY;
  if (m.holder == self) {
    logLine("[SIM-FREERTOS] DEADLOCK: task \"%s\" takes mutex %p that it "
            "already holds. The mutex is not recursive, so %s.",
            taskName(self), static_cast<const void *>(&m),
            forever ? "the device blocks here forever"
                    : "the take fails after its timeout");
  } else {
    logLine("[SIM-FREERTOS] DEADLOCK: task \"%s\" waits for mutex %p held by "
            "\"%s\", which is waiting (directly or through other tasks) for a "
            "mutex \"%s\" holds. %s.",
            taskName(self), static_cast<const void *>(&m), taskName(m.holder),
            taskName(self),
            forever ? "The device blocks here forever"
                    : "The take fails after its timeout");
  }
  fprintf(stderr, "  \"%s\" is waiting at:\n", taskName(self));
  printCurrentTrace();
  printHolderTrace(m);
}

BaseType_t takeMutex(SimMutex &m, uint32_t ticksToWait, bool recursive) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(stateMutex());
  if (!m.holder) {
    acquire(m, self);
    return pdTRUE;
  }
  if (recursive && m.holder == self) {
    ++m.depth;
    return pdTRUE;
  }
  if (ticksToWait == 0)
    return pdFALSE;

  bool reported = false;
  if (waitsForSelf(m, self)) {
    reportDeadlock(m, self, ticksToWait);
    reported = true;
  }

  const auto isFree = [&m] { return m.holder == nullptr; };
  self->waitingOn = &m;
  bool gotIt;
  if (ticksToWait == portMAX_DELAY) {
    gotIt = m.cv.wait_for(lk, kLongWait, isFree);
    if (!gotIt && !reported) {
      logLine("[SIM-FREERTOS] LONG WAIT: task \"%s\" has waited %lld s for "
              "mutex %p held by \"%s\".",
              taskName(self), static_cast<long long>(kLongWait.count()),
              static_cast<void *>(&m), taskName(m.holder));
      fprintf(stderr, "  \"%s\" is waiting at:\n", taskName(self));
      printCurrentTrace();
      printHolderTrace(m);
    }
    if (!gotIt) {
      m.cv.wait(lk, isFree);
      gotIt = true;
    }
  } else {
    gotIt = m.cv.wait_for(lk, ticksToDuration(ticksToWait), isFree);
  }
  self->waitingOn = nullptr;
  if (!gotIt)
    return pdFALSE;
  acquire(m, self);
  return pdTRUE;
}

void release(SimMutex &m) {
  m.holder = nullptr;
  m.depth = 0;
  m.takenAtFrames = 0;
  m.cv.notify_all();
}

} // namespace

SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimMutex(); }

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
  return new SimMutex(SimSemaphoreKind::RecursiveMutex);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
  return storage;
}

SemaphoreHandle_t xSemaphoreCreateBinary() {
  return new SimCountingSemaphore(1, 0);
}

SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t maxCount,
                                           uint32_t initialCount) {
  if (maxCount == 0 || initialCount > maxCount)
    return nullptr;
  return new SimCountingSemaphore(maxCount, initialCount);
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
  if (!sem)
    failAssert("vSemaphoreDelete on a NULL handle", sem);
  if (sem->kind == SimSemaphoreKind::Counting)
    delete static_cast<SimCountingSemaphore *>(sem);
  else
    delete static_cast<SimMutex *>(sem);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait) {
  if (!sem)
    failAssert("xSemaphoreTake on a NULL handle", sem);
  if (sem->kind != SimSemaphoreKind::Counting)
    return takeMutex(*static_cast<SimMutex *>(sem), ticksToWait, false);

  auto &s = *static_cast<SimCountingSemaphore *>(sem);
  std::unique_lock<std::mutex> lk(stateMutex());
  const auto hasToken = [&s] { return s.count > 0; };
  if (ticksToWait == portMAX_DELAY) {
    s.cv.wait(lk, hasToken);
  } else if (!s.cv.wait_for(lk, ticksToDuration(ticksToWait), hasToken)) {
    // ticksToWait == 0 lands here too: wait_for tests the predicate once.
    return pdFALSE;
  }
  --s.count;
  return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
  if (!sem)
    failAssert("xSemaphoreGive on a NULL handle", sem);
  std::lock_guard<std::mutex> lk(stateMutex());
  if (sem->kind == SimSemaphoreKind::Counting) {
    auto &s = *static_cast<SimCountingSemaphore *>(sem);
    if (s.count >= s.max)
      return pdFALSE;
    ++s.count;
    s.cv.notify_all();
    return pdTRUE;
  }
  auto &m = *static_cast<SimMutex *>(sem);
  if (!m.holder)
    return pdFALSE; // Already free: the device reports errQUEUE_FULL.
  if (m.holder != xTaskGetCurrentTaskHandle()) {
    logLine("[SIM-FREERTOS] mutex %p given by \"%s\" but held by \"%s\".",
            static_cast<void *>(&m), taskName(xTaskGetCurrentTaskHandle()),
            taskName(m.holder));
    printHolderTrace(m);
    failAssert("xSemaphoreGive by a task that does not hold the mutex "
               "(xTaskPriorityDisinherit)",
               sem);
  }
  // A plain give frees the mutex even after recursive takes, as on the device.
  release(m);
  return pdTRUE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem,
                                   uint32_t ticksToWait) {
  if (!sem)
    failAssert("xSemaphoreTakeRecursive on a NULL handle", sem);
  if (sem->kind == SimSemaphoreKind::Counting)
    failAssert("xSemaphoreTakeRecursive on a semaphore that is not a mutex",
               sem);
  return takeMutex(*static_cast<SimMutex *>(sem), ticksToWait, true);
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem) {
  if (!sem)
    failAssert("xSemaphoreGiveRecursive on a NULL handle", sem);
  if (sem->kind == SimSemaphoreKind::Counting)
    failAssert("xSemaphoreGiveRecursive on a semaphore that is not a mutex",
               sem);
  std::lock_guard<std::mutex> lk(stateMutex());
  auto &m = *static_cast<SimMutex *>(sem);
  // The device returns pdFAIL, without asserting, when the caller is not
  // the holder.
  if (m.holder != xTaskGetCurrentTaskHandle())
    return pdFALSE;
  if (--m.depth == 0)
    release(m);
  return pdTRUE;
}

TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) {
  if (!sem || sem->kind == SimSemaphoreKind::Counting)
    return nullptr;
  std::lock_guard<std::mutex> lk(stateMutex());
  return static_cast<SimMutex *>(sem)->holder;
}

uint32_t uxSemaphoreGetCount(SemaphoreHandle_t sem) {
  if (!sem)
    failAssert("uxSemaphoreGetCount on a NULL handle", sem);
  std::lock_guard<std::mutex> lk(stateMutex());
  if (sem->kind == SimSemaphoreKind::Counting)
    return static_cast<SimCountingSemaphore *>(sem)->count;
  return static_cast<SimMutex *>(sem)->holder ? 0 : 1;
}

int xQueuePeek(SemaphoreHandle_t sem, void *, uint32_t) {
  return uxSemaphoreGetCount(sem) > 0 ? pdTRUE : pdFALSE;
}
