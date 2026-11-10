#ifdef NDEBUG
#undef NDEBUG
#endif

// Checks that the FreeRTOS shim follows the device's semaphore rules.
// Run with an argument to check a case that must abort:
//   give-other  a task gives a mutex another task holds
//   null-take   xSemaphoreTake on a NULL handle

#include <freertos/semphr.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <unistd.h>

#include "SimLog.h"

using namespace std::chrono;

static bool logged(const char *pattern) {
  std::string line;
  return sim_log::findSince(0, std::regex(pattern), &line);
}

static void mutexIsNotRecursive() {
  auto m = xSemaphoreCreateMutex();
  assert(xSemaphoreTake(m, portMAX_DELAY) == pdTRUE);
  // A second take by the holder fails after its timeout, as on the device.
  const auto start = steady_clock::now();
  assert(xSemaphoreTake(m, pdMS_TO_TICKS(50)) == pdFALSE);
  assert(steady_clock::now() - start >= milliseconds(50));
  assert(logged("\\[SIM-FREERTOS\\] DEADLOCK: task \"main\" takes mutex"));
  assert(xSemaphoreTake(m, 0) == pdFALSE);
  assert(xSemaphoreGive(m) == pdTRUE);
  // Giving a free mutex fails without asserting.
  assert(xSemaphoreGive(m) == pdFALSE);
  assert(xSemaphoreGetMutexHolder(m) == nullptr);
  vSemaphoreDelete(m);
}

static void selfDeadlockBlocksForever() {
  auto m = xSemaphoreCreateMutex();
  std::atomic<bool> returned{false};
  std::thread task([&] {
    assert(xSemaphoreTake(m, portMAX_DELAY) == pdTRUE);
    xSemaphoreTake(m, portMAX_DELAY);
    returned = true;
  });
  std::this_thread::sleep_for(milliseconds(200));
  assert(!returned.load());
  assert(logged("DEADLOCK: task \"host-thread\" takes mutex"));
  task.detach(); // Stays blocked, like the device task.
}

static void cycleIsReported() {
  auto a = xSemaphoreCreateMutex();
  auto b = xSemaphoreCreateMutex();
  std::atomic<int> stage{0};
  std::thread other([&] {
    assert(xSemaphoreTake(b, portMAX_DELAY) == pdTRUE);
    stage = 1;
    while (stage.load() != 2)
      std::this_thread::yield();
    xSemaphoreTake(a, portMAX_DELAY); // Waits for main.
  });
  assert(xSemaphoreTake(a, portMAX_DELAY) == pdTRUE);
  while (stage.load() != 1)
    std::this_thread::yield();
  stage = 2;
  std::this_thread::sleep_for(milliseconds(50));
  // main now waits for b while "other" waits for a.
  assert(xSemaphoreTake(b, pdMS_TO_TICKS(50)) == pdFALSE);
  assert(logged("DEADLOCK: task \"main\" waits for mutex .* held by "
                "\"host-thread\""));
  other.detach();
}

static void mutexExcludesOtherTasks() {
  auto m = xSemaphoreCreateMutex();
  assert(xSemaphoreTake(m, portMAX_DELAY) == pdTRUE);
  std::atomic<bool> entered{false};
  std::thread task([&] {
    assert(xSemaphoreTake(m, 0) == pdFALSE);
    assert(xSemaphoreTake(m, pdMS_TO_TICKS(20)) == pdFALSE);
    assert(xSemaphoreTake(m, portMAX_DELAY) == pdTRUE);
    entered = true;
    assert(xSemaphoreGive(m) == pdTRUE);
  });
  std::this_thread::sleep_for(milliseconds(60));
  assert(!entered.load());
  assert(xQueuePeek(m, nullptr, 0) == pdFALSE);
  assert(xSemaphoreGive(m) == pdTRUE);
  task.join();
  assert(entered.load());
  assert(xQueuePeek(m, nullptr, 0) == pdTRUE);
}

static void recursiveMutex() {
  auto m = xSemaphoreCreateRecursiveMutex();
  assert(xSemaphoreTakeRecursive(m, portMAX_DELAY) == pdTRUE);
  assert(xSemaphoreTakeRecursive(m, portMAX_DELAY) == pdTRUE);
  std::thread task([&] {
    assert(xSemaphoreTakeRecursive(m, 0) == pdFALSE);
    // Not the holder: fails without asserting.
    assert(xSemaphoreGiveRecursive(m) == pdFALSE);
  });
  task.join();
  assert(xSemaphoreGiveRecursive(m) == pdTRUE);
  assert(xSemaphoreGetMutexHolder(m) == xTaskGetCurrentTaskHandle());
  assert(xSemaphoreGiveRecursive(m) == pdTRUE);
  assert(xSemaphoreGetMutexHolder(m) == nullptr);
  assert(xSemaphoreGiveRecursive(m) == pdFALSE);
}

static void binaryAndCounting() {
  auto bin = xSemaphoreCreateBinary();
  assert(uxSemaphoreGetCount(bin) == 0);
  assert(xSemaphoreTake(bin, pdMS_TO_TICKS(10)) == pdFALSE);
  assert(xSemaphoreGive(bin) == pdTRUE);
  assert(xSemaphoreGive(bin) == pdFALSE); // Full.
  std::thread taker([&] { assert(xSemaphoreTake(bin, portMAX_DELAY)); });
  taker.join();
  // Given from a task that never took it.
  std::thread giver([&] { assert(xSemaphoreGive(bin) == pdTRUE); });
  giver.join();
  assert(xSemaphoreTake(bin, 0) == pdTRUE);

  assert(xSemaphoreCreateCounting(2, 3) == nullptr);
  auto cnt = xSemaphoreCreateCounting(3, 2);
  assert(uxSemaphoreGetCount(cnt) == 2);
  assert(xSemaphoreGive(cnt) == pdTRUE);
  assert(xSemaphoreGive(cnt) == pdFALSE);
  for (int i = 0; i < 3; ++i)
    assert(xSemaphoreTake(cnt, 0) == pdTRUE);
  assert(xSemaphoreTake(cnt, 0) == pdFALSE);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "give-other") == 0) {
    auto m = xSemaphoreCreateMutex();
    std::atomic<bool> taken{false};
    std::thread holder([&] {
      xSemaphoreTake(m, portMAX_DELAY);
      taken = true;
      std::this_thread::sleep_for(seconds(10));
    });
    holder.detach();
    while (!taken.load())
      std::this_thread::yield();
    xSemaphoreGive(m);
    return 0; // Not reached.
  }
  if (argc > 1 && strcmp(argv[1], "null-take") == 0) {
    xSemaphoreTake(nullptr, 0);
    return 0; // Not reached.
  }
  mutexIsNotRecursive();
  mutexExcludesOtherTasks();
  recursiveMutex();
  binaryAndCounting();
  cycleIsReported();
  selfDeadlockBlocksForever();
  std::puts("FreeRTOS semaphore rules hold");
  std::fflush(stdout);
  _exit(0); // Blocked test threads stay blocked.
}
