/****************************************************************************
 * apps/testing/ostest/signest.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "ostest.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WAKEUP_SIGNAL SIGRTMIN
#define SIGVALUE_INT  42

/****************************************************************************
 * Private Data
 ****************************************************************************/

static sem_t g_waiter_sem;
static sem_t g_interferer_sem;
static volatile bool g_waiter_running;
static volatile bool g_interferer_running;
static volatile bool g_done;
static volatile int g_nestlevel;

static volatile int g_even_handled;
static volatile int g_odd_handled;
static volatile int g_even_nested;
static volatile int g_odd_nested;

static volatile int g_nest_level;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool signest_catchable(int signo)
{
#ifdef CONFIG_SIG_SIGSTOP_ACTION
  if (signo == SIGSTOP || signo == SIGCONT)
    {
      return false;
    }
#endif

#ifdef CONFIG_SIG_SIGKILL_ACTION
  if (signo == SIGKILL)
    {
      return false;
    }
#endif

  return true;
}

static void waiter_action(int signo)
{
  int nest_level;

  sched_lock();
  nest_level = g_nest_level++;
  sched_unlock();

  if ((signo & 1) != 0)
    {
      g_odd_handled++;
      if (nest_level > 0)
        {
          g_odd_nested++;
        }
    }
  else
    {
      g_even_handled++;
      if (nest_level > 0)
        {
          g_even_nested++;
        }
    }

  g_nest_level = nest_level;
}

static int waiter_main(int argc, char *argv[])
{
  sigset_t set;
  struct sigaction act;
  int ret;
  int i;

  printf("waiter_main: Waiter started\n");
  printf("waiter_main: Setting signal mask\n");

  sigemptyset(&set);
  ret = sigprocmask(SIG_SETMASK, &set, NULL);
  if (ret < 0)
    {
      printf("waiter_main: ERROR sigprocmask failed: %d\n", errno);
      ASSERT(false);
      return EXIT_FAILURE;
    }

  printf("waiter_main: Registering signal handler\n");

  act.sa_handler = waiter_action;
  act.sa_flags   = 0;

  sigemptyset(&act.sa_mask);
  for (i = 1; i < MAX_SIGNO; i += 2)
    {
      if (signest_catchable(i))
        {
          sigaddset(&act.sa_mask, i);
        }
    }

  for (i = 1; i < MAX_SIGNO; i++)
    {
      if (signest_catchable(i))
        {
          ret = sigaction(i, &act, NULL);
          if (ret < 0)
            {
              printf("waiter_main: WARNING sigaction failed with %d\n",
                     errno);
              return EXIT_FAILURE;
            }
        }
    }

  /* Now just loop until the test completes */

  printf("waiter_main: Waiting on semaphore\n");
  FFLUSH();

  g_waiter_running = true;
  while (!g_done)
    {
      ret = sem_wait(&g_waiter_sem);
    }

  /* Just exit, the system should clean up the signal handlers */

  g_waiter_running = false;
  return EXIT_SUCCESS;
}

static int interfere_main(int argc, char *argv[])
{
  /* Now just loop staying in the way as much as possible */

  printf("interfere_main: Waiting on semaphore\n");
  FFLUSH();

  g_interferer_running = true;
  while (!g_done)
    {
      sem_wait(&g_interferer_sem);
    }

  /* Just exit, the system should clean up the signal handlers */

  g_interferer_running = false;
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void signest_test(void)
{
  struct sched_param param;
  pid_t waiterpid;
  pid_t interferepid;
  int total_signals;
  int total_handled;
  int total_nested;
  int even_signals;
  int odd_signals;
  int prio;
  int ret;
  int i;
  int j;

  sem_init(&g_waiter_sem, 0, 0);
  sem_init(&g_interferer_sem, 0, 0);
  g_waiter_running = false;
  g_interferer_running = false;
  g_done = false;
  g_nestlevel = 0;

  even_signals = 0;
  odd_signals = 0;

  g_even_handled = 0;
  g_odd_handled = 0;
  g_even_nested = 0;
  g_odd_nested = 0;

  g_nest_level = 0;

  ret = sched_getparam (0, &param);
  if (ret < 0)
    {
      printf("signest_test: ERROR sched_getparam() failed\n");
      ASSERT(false);
      param.sched_priority = PTHREAD_DEFAULT_PRIORITY;
    }

  /* Start waiter thread  */

  prio = param.sched_priority + 1;
  printf("signest_test: Starting signal waiter task at priority %d\n", prio);
  waiterpid = task_create("waiter", param.sched_priority,
                           STACKSIZE, waiter_main, NULL);
  if (waiterpid == ERROR)
    {
      printf("signest_test: ERROR failed to start waiter_main\n");
      ASSERT(false);
      return;
    }

  printf("signest_test: Started waiter_main pid=%d\n", waiterpid);

  /* Start interfering thread  */

  prio++;
  printf("signest_test: Starting interfering task at priority %d\n", prio);
  interferepid = task_create("interfere", param.sched_priority,
                           STACKSIZE, interfere_main, NULL);
  if (interferepid == ERROR)
    {
      printf("signest_test: ERROR failed to start interfere_main\n");
      ASSERT(false);
      goto errout_with_waiter;
    }

  printf("signest_test: Started interfere_main pid=%d\n", interferepid);

  /* Wait a bit */

  FFLUSH();
  usleep(500 * 1000L);

  /* Then signal the waiter thread with back-to-back signals, one masked
   * and the other unmasked.
   */

  for (i = 0; i < 10; i++)
    {
      for (j = 1; j < MAX_SIGNO; j += 2)
        {
          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          usleep(10 * 1000L);

          /* Even then odd */

          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          usleep(10 * 1000L);
        }
    }

  /* Check the test results so far */

  total_signals = odd_signals + even_signals;
  total_handled = g_odd_handled + g_even_handled;
  total_nested  = g_odd_nested + g_even_nested;

  printf("signest_test: Simple case:\n");
  printf("  Total signalled %-3d  Odd=%-3d Even=%-3d\n",
         total_signals, odd_signals, even_signals);
  printf("  Total handled   %-3d  Odd=%-3d Even=%-3d\n",
         total_handled, g_odd_handled, g_even_handled);
  printf("  Total nested    %-3d  Odd=%-3d Even=%-3d\n",
         total_nested, g_odd_nested, g_even_nested);

  /* Then signal the waiter thread with two signals pending.  The
   * sched_lock() assures that the first signal was not processed
   * before the second was delivered.
   */

  for (i = 0; i < 10; i++)
    {
      for (j = 1; j < MAX_SIGNO; j += 2)
        {
          /* Odd then even */

          sched_lock();

          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          sched_unlock();

          usleep(10 * 1000L);

          /* Even then odd */

          sched_lock();

          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          sched_unlock();

          usleep(10 * 1000L);
        }
    }

  /* Check the test results so far */

  total_signals = odd_signals + even_signals;
  total_handled = g_odd_handled + g_even_handled;
  total_nested  = g_odd_nested + g_even_nested;

  printf("signest_test: With task locking\n");
  printf("  Total signalled %-3d  Odd=%-3d Even=%-3d\n",
         total_signals, odd_signals, even_signals);
  printf("  Total handled   %-3d  Odd=%-3d Even=%-3d\n",
         total_handled, g_odd_handled, g_even_handled);
  printf("  Total nested    %-3d  Odd=%-3d Even=%-3d\n",
         total_nested, g_odd_nested, g_even_nested);

  /* Then do it all over again with the interfering thread. */

  for (i = 0; i < 10; i++)
    {
      for (j = 1; j < MAX_SIGNO; j += 2)
        {
          /* Odd then even */

          sched_lock();

          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          sem_post(&g_interferer_sem);

          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          sched_unlock();

          usleep(10 * 1000L);

          /* Even then odd */

          sched_lock();
          if (signest_catchable(j + 1))
            {
              kill(waiterpid, j + 1);
              even_signals++;
            }

          sem_post(&g_interferer_sem);

          if (signest_catchable(j))
            {
              kill(waiterpid, j);
              odd_signals++;
            }

          sched_unlock();

          usleep(10 * 1000L);
        }
    }

  /* Stop the threads */

errout_with_waiter:
  g_done = true;
  sem_post(&g_waiter_sem);
  sem_post(&g_interferer_sem);
  usleep(500 * 1000L);

  /* Check the final test results */

  total_signals = odd_signals + even_signals;
  total_handled = g_odd_handled + g_even_handled;
  total_nested  = g_odd_nested + g_even_nested;

  printf("signest_test: With intefering thread\n");
  printf("  Total signalled %-3d  Odd=%-3d Even=%-3d\n",
         total_signals, odd_signals, even_signals);
  printf("  Total handled   %-3d  Odd=%-3d Even=%-3d\n",
         total_handled, g_odd_handled, g_even_handled);
  printf("  Total nested    %-3d  Odd=%-3d Even=%-3d\n",
         total_nested, g_odd_nested, g_even_nested);

  /* Check for error */

  if (g_waiter_running)
    {
      printf("signest_test: ERROR waiter is still running\n");
      ASSERT(false);
    }

  if (g_interferer_running)
    {
      printf("signest_test: ERROR interferer is still running\n");
      ASSERT(false);
    }

  if (total_signals != total_handled)
    {
      printf("signest_test: ERROR only %d of %d signals were handled\n",
             total_handled, total_signals);
      ASSERT(false);
    }

  if (odd_signals != g_odd_handled)
    {
      printf("signest_test: ERROR only %d of %d ODD signals were handled\n",
             g_odd_handled, odd_signals);
      ASSERT(false);
    }

  if (even_signals != g_even_handled)
    {
      printf("signest_test: ERROR only %d of %d EVEN signals were handled\n",
             g_even_handled, even_signals);
      ASSERT(false);
    }

  if (g_odd_nested > 0)
    {
      printf("signest_test: ERROR %d ODD signals were nested\n",
             g_odd_nested);
      ASSERT(false);
    }

  sem_destroy(&g_waiter_sem);
  sem_destroy(&g_interferer_sem);

  printf("signest_test: done\n");
  FFLUSH();
}
