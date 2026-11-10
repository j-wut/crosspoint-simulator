#pragma once
#include <condition_variable>
#include <cstdint>

#include "FreeRTOS.h"
#include "task.h"

// Semaphores with the same rules as FreeRTOS on the device (ESP-IDF):
//
// - xSemaphoreCreateMutex is NOT recursive. A task that takes a mutex it
//   already holds blocks until ticksToWait runs out; with portMAX_DELAY it
//   blocks forever, as the device does.
// - xSemaphoreCreateRecursiveMutex is recursive only through
//   xSemaphoreTakeRecursive / xSemaphoreGiveRecursive.
// - Giving a mutex held by another task fails configASSERT on the device and
//   aborts here. Giving a mutex nobody holds returns pdFALSE.
// - Binary and counting semaphores have no owner. Any task may give them, and a
//   give to a full semaphore returns pdFALSE.
// - A NULL handle fails configASSERT on the device and aborts here.
//
// A blocked take that can never finish (a task waiting for itself, or a cycle
// of tasks waiting for each other) and a mutex wait longer than 5 s are logged
// with backtraces of the waiting task and of where the holder took the mutex.
// The log line starts with "[SIM-FREERTOS]", so input scripts can WAIT for it.
// The state lives in semphr.cpp, behind one host mutex.

enum class SimSemaphoreKind { Mutex, RecursiveMutex, Counting };

struct SimSemaphoreBase {
  SimSemaphoreKind kind;
  std::condition_variable cv;
  explicit SimSemaphoreBase(SimSemaphoreKind k) : kind(k) {}
};

struct SimMutex : SimSemaphoreBase {
  explicit SimMutex(SimSemaphoreKind k = SimSemaphoreKind::Mutex)
      : SimSemaphoreBase(k) {}
  TaskHandle_t holder = nullptr;
  uint32_t depth = 0; // Recursive takes by the holder.
  // Where the holder took the mutex, for deadlock reports.
  static constexpr int kTraceDepth = 24;
  void *takenAt[kTraceDepth] = {};
  int takenAtFrames = 0;
};

struct SimCountingSemaphore : SimSemaphoreBase {
  SimCountingSemaphore(uint32_t max, uint32_t initial)
      : SimSemaphoreBase(SimSemaphoreKind::Counting), count(initial), max(max) {
  }
  uint32_t count;
  const uint32_t max;
};

typedef SimSemaphoreBase *SemaphoreHandle_t;
// The static API currently used by firmware is xSemaphoreCreateMutexStatic.
using StaticSemaphore_t = SimMutex;

SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
// Created empty, as on the device: the first take waits for a give.
SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t maxCount,
                                           uint32_t initialCount);
void vSemaphoreDelete(SemaphoreHandle_t sem);

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, uint32_t ticksToWait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem);

TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem);
// Mutex: 1 when free, 0 when held. Counting/binary: tokens available.
uint32_t uxSemaphoreGetCount(SemaphoreHandle_t sem);
// On a semaphore handle: pdTRUE when a take would succeed now.
int xQueuePeek(SemaphoreHandle_t sem, void *buffer, uint32_t ticksToWait);
