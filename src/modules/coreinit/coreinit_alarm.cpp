#include "coreinit.h"
#include "coreinit_alarm.h"
#include "coreinit_core.h"
#include "coreinit_spinlock.h"
#include "coreinit_scheduler.h"
#include "coreinit_thread.h"
#include "coreinit_memheap.h"
#include "coreinit_time.h"
#include "coreinit_queue.h"
#include "utils/wfunc_call.h"
#include "processor.h"

static OSSpinLock *
gAlarmLock;

static OSAlarmQueue *
gAlarmQueue[CoreCount];

const uint32_t
OSAlarm::Tag;

const uint32_t
OSAlarmQueue::Tag;

static BOOL
OSCancelAlarmNoLock(OSAlarm *alarm)
{
   if (alarm->state != OSAlarmState::Set) {
      return FALSE;
   }

   alarm->state = OSAlarmState::Cancelled;
   alarm->nextFire = 0;
   alarm->period = 0;

   if (alarm->alarmQueue) {
      OSEraseFromQueue<OSAlarmQueue>(alarm->alarmQueue, alarm);
   }

   OSWakeupThread(&alarm->threadQueue);
   return TRUE;
}

BOOL
OSCancelAlarm(OSAlarm *alarm)
{
   ScopedSpinLock lock(gAlarmLock);
   return OSCancelAlarmNoLock(alarm);
}

void
OSCancelAlarms(uint32_t alarmTag)
{
   ScopedSpinLock lock(gAlarmLock);

   // Cancel all alarms with matching alarmTag
   for (auto i = 0u; i < 3; ++i) {
      auto queue = gAlarmQueue[i];

      for (OSAlarm *alarm = queue->head; alarm; ) {
         auto next = alarm->link.next;

         if (alarm->alarmTag == alarmTag) {
            OSCancelAlarmNoLock(alarm);
         }

         alarm = next;
      }
   }
}

void
OSCreateAlarm(OSAlarm *alarm)
{
   OSCreateAlarmEx(alarm, nullptr);
}

void
OSCreateAlarmEx(OSAlarm *alarm, const char *name)
{
   memset(alarm, 0, sizeof(OSAlarm));
   alarm->tag = OSAlarm::Tag;
   alarm->name = name;
   OSInitThreadQueueEx(&alarm->threadQueue, alarm);
}

void *
OSGetAlarmUserData(OSAlarm *alarm)
{
   return alarm->userData;
}

void
OSInitAlarmQueue(OSAlarmQueue *queue)
{
   memset(queue, 0, sizeof(OSAlarmQueue));
   queue->tag = OSAlarmQueue::Tag;
}

BOOL
OSSetAlarm(OSAlarm *alarm, OSTime time, AlarmCallback callback)
{
   return OSSetPeriodicAlarm(alarm, OSGetTime() + time, 0, callback);
}

BOOL
OSSetPeriodicAlarm(OSAlarm *alarm, OSTime start, OSTime interval, AlarmCallback callback)
{
   ScopedSpinLock lock(gAlarmLock);

   // Set alarm
   alarm->nextFire = start;
   alarm->callback = callback;
   alarm->period = interval;
   alarm->context = nullptr;
   alarm->state = OSAlarmState::Set;

   // Erase from old alarm queue
   if (alarm->alarmQueue) {
      OSEraseFromQueue(static_cast<OSAlarmQueue*>(alarm->alarmQueue), alarm);
   }

   // Add to this core's alarm queue
   auto core = OSGetCoreId();
   auto queue = gAlarmQueue[core];
   alarm->alarmQueue = queue;
   OSAppendQueue(queue, alarm);

   // Set the interrupt timer in processor
   gProcessor.setInterruptTimer(core, OSTimeToChrono(alarm->nextFire));
   return TRUE;
}

void
OSSetAlarmTag(OSAlarm *alarm, uint32_t alarmTag)
{
   OSUninterruptibleSpinLock_Acquire(gAlarmLock);
   alarm->alarmTag = alarmTag;
   OSUninterruptibleSpinLock_Release(gAlarmLock);
}

void
OSSetAlarmUserData(OSAlarm *alarm, void *data)
{
   OSUninterruptibleSpinLock_Acquire(gAlarmLock);
   alarm->userData = data;
   OSUninterruptibleSpinLock_Release(gAlarmLock);
}

BOOL
OSWaitAlarm(OSAlarm *alarm)
{
   OSLockScheduler();
   OSUninterruptibleSpinLock_Acquire(gAlarmLock);
   BOOL result = FALSE;
   assert(alarm);
   assert(alarm->tag == OSAlarm::Tag);

   if (alarm->state != OSAlarmState::Set) {
      OSUninterruptibleSpinLock_Release(gAlarmLock);
      OSUnlockScheduler();
      return FALSE;
   }

   OSSleepThreadNoLock(&alarm->threadQueue);
   OSUninterruptibleSpinLock_Release(gAlarmLock);
   OSRescheduleNoLock();

   OSUninterruptibleSpinLock_Acquire(gAlarmLock);

   if (alarm->state != OSAlarmState::Cancelled) {
      result = TRUE;
   }

   OSUninterruptibleSpinLock_Release(gAlarmLock);
   OSUnlockScheduler();
   return result;
}

static void
OSTriggerAlarmNoLock(OSAlarm *alarm, OSContext *context)
{
   alarm->context = context;

   if (alarm->period) {
      alarm->nextFire = OSGetTime() + alarm->period;
      alarm->state = OSAlarmState::Set;
   } else {
      alarm->nextFire = 0;
      alarm->state = OSAlarmState::None;
      OSEraseFromQueue<OSAlarmQueue>(alarm->alarmQueue, alarm);
   }

   if (alarm->callback) {
      OSUninterruptibleSpinLock_Release(gAlarmLock);
      alarm->callback(alarm, context);
      OSUninterruptibleSpinLock_Acquire(gAlarmLock);
   }

   OSWakeupThreadNoLock(&alarm->threadQueue);
}

void
OSCheckAlarms(uint32_t core, OSContext *context)
{
   auto queue = gAlarmQueue[core];
   auto now = OSGetTime();
   auto next = std::chrono::time_point<std::chrono::system_clock>::max();

   OSLockScheduler();
   OSUninterruptibleSpinLock_Acquire(gAlarmLock);

   for (OSAlarm *alarm = queue->head; alarm; ) {
      auto nextAlarm = alarm->link.next;

      // Trigger alarm if it is time
      if (alarm->nextFire <= now && alarm->state != OSAlarmState::Cancelled) {
         OSTriggerAlarmNoLock(alarm, context);
      }

      // Set next timer if alarm is set
      if (alarm->state == OSAlarmState::Set && alarm->nextFire) {
         auto nextFire = OSTimeToChrono(alarm->nextFire);

         if (nextFire < next) {
            next = nextFire;
         }
      }

      alarm = nextAlarm;
   }

   OSUninterruptibleSpinLock_Release(gAlarmLock);
   OSUnlockScheduler();
   gProcessor.setInterruptTimer(core, next);
}

void
CoreInit::registerAlarmFunctions()
{
   RegisterKernelFunction(OSCancelAlarm);
   RegisterKernelFunction(OSCancelAlarms);
   RegisterKernelFunction(OSCreateAlarm);
   RegisterKernelFunction(OSCreateAlarmEx);
   RegisterKernelFunction(OSGetAlarmUserData);
   RegisterKernelFunction(OSSetAlarm);
   RegisterKernelFunction(OSSetPeriodicAlarm);
   RegisterKernelFunction(OSSetAlarmTag);
   RegisterKernelFunction(OSSetAlarmUserData);
   RegisterKernelFunction(OSWaitAlarm);
}

void
CoreInit::initialiseAlarm()
{
   gAlarmLock = OSAllocFromSystem<OSSpinLock>();

   for (auto i = 0u; i < CoreCount; ++i) {
      gAlarmQueue[i] = OSAllocFromSystem<OSAlarmQueue>();
      OSInitAlarmQueue(gAlarmQueue[i]);
   }
}
