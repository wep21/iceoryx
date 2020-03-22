// Copyright (c) 2020 by Robert Bosch GmbH. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_utils/platform/time.hpp"

static std::chrono::nanoseconds getNanoSeconds(const timespec& value)
{
    static constexpr uint64_t NANOSECONDS = 1000000000;
    return std::chrono::nanoseconds(static_cast<uint64_t>(value.tv_sec) * NANOSECONDS
                                    + static_cast<uint64_t>(value.tv_nsec));
}

static void stopTimerThread(timer_t timerid)
{
    timerid->keepRunning.store(false, std::memory_order_relaxed);
    timerid->parameter.wakeup.notify_one();
    if (timerid->thread.joinable())
    {
        timerid->thread.join();
    }
}

static bool waitForExecution(timer_t timerid)
{
    using timePoint_t = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;
    std::unique_lock<std::mutex> ulock(timerid->parameter.mutex);

    if (timerid->parameter.isTimerRunning)
    {
        timespec waitUntil;
        clock_gettime(CLOCK_REALTIME, &waitUntil);
        waitUntil.tv_sec += timerid->parameter.timeParameters.it_value.tv_sec;
        waitUntil.tv_nsec += timerid->parameter.timeParameters.it_value.tv_nsec;
        timerid->parameter.wakeup.wait_until(ulock, timePoint_t(getNanoSeconds(waitUntil)), [timerid] {
            return !timerid->parameter.isTimerRunning || !timerid->keepRunning.load(std::memory_order_relaxed);
        });
    }
    else
    {
        timerid->parameter.wakeup.wait(ulock, [timerid] {
            return timerid->parameter.isTimerRunning || !timerid->keepRunning.load(std::memory_order_relaxed);
        });
    }

    return timerid->parameter.isTimerRunning;
}

static void
setTimeParameters(timer_t timerid, const itimerspec& timeParameters, const bool runOnce, const bool isTimerRunning)
{
    std::lock_guard<std::mutex> l(timerid->parameter.mutex);
    clock_gettime(CLOCK_REALTIME, &timerid->parameter.startTime);
    timerid->parameter.timeParameters = timeParameters;
    timerid->parameter.runOnce = runOnce;
    timerid->parameter.wasCallbackCalled = false;
    timerid->parameter.isTimerRunning = isTimerRunning;
}

int timer_create(clockid_t clockid, struct sigevent* sevp, timer_t* timerid)
{
    timer_t timer = new appleTimer_t();
    timer->callback = sevp->sigev_notify_function;
    timer->callbackParameter = sevp->sigev_value;

    timer->thread = std::thread([timer] {
        while (timer->keepRunning.load(std::memory_order_relaxed))
        {
            if (waitForExecution(timer))
            {
                timer->parameter.mutex.lock();
                bool doCallCallback =
                    !timer->parameter.runOnce || timer->parameter.runOnce && !timer->parameter.wasCallbackCalled;
                timer->parameter.mutex.unlock();
                if (doCallCallback)
                {
                    timer->callback(timer->callbackParameter);

                    std::lock_guard<std::mutex> l(timer->parameter.mutex);
                    timer->parameter.wasCallbackCalled = true;
                }
            }
        }
    });

    *timerid = timer;
    return 0;
}

int timer_delete(timer_t timerid)
{
    stopTimerThread(timerid);
    delete timerid;
    return 0;
}

int timer_settime(timer_t timerid, int flags, const struct itimerspec* new_value, struct itimerspec* old_value)
{
    // disarm timer
    if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0)
    {
        setTimeParameters(timerid, *new_value, false, false);
    }
    // run once
    else if (new_value->it_interval.tv_sec == 0 && new_value->it_interval.tv_nsec == 0)
    {
        setTimeParameters(timerid, *new_value, true, true);
    }
    // run periodically
    else
    {
        setTimeParameters(timerid, *new_value, false, true);
    }
    timerid->parameter.wakeup.notify_one();

    return 0;
}

int timer_gettime(timer_t timerid, struct itimerspec* curr_value)
{
    timespec currentTime, startTime;
    clock_gettime(CLOCK_REALTIME, &currentTime);
    {
        std::lock_guard<std::mutex> l(timerid->parameter.mutex);
        curr_value->it_interval = timerid->parameter.timeParameters.it_interval;
        startTime = timerid->parameter.startTime;
    }
    curr_value->it_value.tv_sec = curr_value->it_interval.tv_sec - (currentTime.tv_sec - startTime.tv_sec);
    curr_value->it_value.tv_nsec = curr_value->it_interval.tv_nsec - (currentTime.tv_nsec - startTime.tv_nsec);

    return 0;
}

int timer_getoverrun(timer_t timerid)
{
    return 0;
}
