/* Copyright (C) 2020 Abdulrahman Semrie <hsamireh@gmail.com>
 * Copyright (C) 2020 Aleix Conchillo Flaqué <aconchillo@gmail.com>
 * Copyright (C) 2016 Andy Wingo <wingo@pobox.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */



#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <event2/event.h>
#include <libguile.h>

struct event_data
{
  int fd;
  short event;
};

struct wait_data
{
  int rv;
  struct event_data *events;
  int maxevents;
};

struct loop_data
{
  struct event_base *base;
  int64_t timeout;
};

static void
free_evb (void* ev)
{
  struct event_base *base = ev;
  event_base_free (base);
}

static void
cb_func (evutil_socket_t fd, short what, void *arg)
{
  struct wait_data* data = arg;
  int rv = data->rv;

  if (rv == data->maxevents)
    {
      fprintf (stderr, "max events fired on [%d], ignoring.\n", fd);
      return;
    }

  struct event_data ev_data = { fd, what };
  int sz = sizeof (struct event_data);
  memcpy (data->events + (rv * sz), &ev_data, sz);

  data->rv += 1;
}

static SCM
scm_primitive_event_wake (SCM wakefd)
#define FUNC_NAME "primitive-event-wake"
{
  int c_fd;
  char zero = 0;

  c_fd = scm_to_int (wakefd);

  if (write (c_fd, &zero, 1) <= 0 && errno != EWOULDBLOCK && errno != EAGAIN)
    SCM_SYSERROR;

  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME

static SCM
scm_primitive_create_event_base (SCM eventsv)
#define FUNC_NAME "primitive-create-event-base"
{
  struct event_base *base;
  struct wait_data *data;

  if ((base = event_base_new ()) == NULL)
    scm_syserror ("couldn't instantiate event_base");

  data = (struct wait_data *) scm_gc_malloc (sizeof (struct wait_data),
                                             "wait_data");
  data->rv = 0;
  data->events = (struct event_data *) SCM_BYTEVECTOR_CONTENTS (eventsv);
  data->maxevents = SCM_BYTEVECTOR_LENGTH (eventsv) / sizeof (struct event_data);

  return scm_list_2 (scm_from_pointer (base, free_evb),
                     scm_from_pointer (data, NULL));
}
#undef FUNC_NAME

static SCM
scm_primitive_add_event (SCM lst, SCM fd, SCM ev)
#define FUNC_NAME "primitive-add-event"
{
  int c_fd;
  short c_ev;
  struct event *event;
  struct event_base *base;
  struct wait_data *data;

  c_fd = scm_to_int (fd);
  c_ev = scm_to_short (ev);

  base =
    (struct event_base *) scm_to_pointer (scm_list_ref (lst, scm_from_int (0)));
  data =
    (struct wait_data *) scm_to_pointer (scm_list_ref (lst, scm_from_int (1)));

  if (c_ev & EV_PERSIST)
    {
      event = event_new (base, c_fd, c_ev, cb_func, data);
      event_add (event, NULL);
    }
  else
    event_base_once (base, c_fd, c_ev, cb_func, data, NULL);

  return SCM_UNSPECIFIED;
}

#undef FUNC_NAME

static uint64_t time_units_per_microsec;

static void*
run_event_loop (void *p)
{
  int microsec = 0;
  struct timeval tv;

  struct loop_data *data = p;

  if (data->timeout < 0)
    microsec = -1;
  else if (data->timeout >= 0)
    {
      microsec = data->timeout / time_units_per_microsec;
      tv.tv_sec = 0;
      tv.tv_usec = microsec;
    }

  if (microsec >= 0)
    event_base_loopexit (data->base, &tv);

  event_base_loop (data->base, EVLOOP_ONCE);

  return NULL;
}

static SCM
scm_primitive_event_loop (SCM lst, SCM wakefd, SCM wokefd, SCM timeout)
#define FUNC_NAME "primitive-event-loop"
{
  int c_wakefd;
  int c_wokefd;
  int result = 0;
  int64_t c_timeout;

  c_wakefd = scm_to_int (wakefd);
  c_wokefd = scm_to_int (wokefd);
  c_timeout = scm_to_int64 (timeout);

  struct event_base *base =
    (struct event_base *) scm_to_pointer (scm_list_ref (lst, scm_from_int (0)));
  struct wait_data *data =
    (struct wait_data *) scm_to_pointer (scm_list_ref (lst, scm_from_int (1)));

  if (data == NULL)
    scm_wrong_type_arg_msg ("event-loop", 1,
                            scm_list_ref (lst, scm_from_int (1)),
                            "event wait_data is null");

  if (c_timeout != 0 && scm_c_prepare_to_wait_on_fd (c_wakefd))
    data->rv = 0;
  else
    {
      struct loop_data loop_data = { base, c_timeout };

      if (c_timeout != 0)
        scm_without_guile (run_event_loop, &loop_data);
      else
        run_event_loop (&loop_data);

      if (c_timeout != 0)
        scm_c_wait_finished ();

      for (int i = 0; i < data->rv; i++) {
        if (data->events[i].fd == c_wokefd)
          {
            char zeroes[32];
            /* Remove wake fd from result set.  */
            data->rv--;
            memmove (data->events + i,
                     data->events + i + 1,
                     (data->rv - i) * sizeof (struct event_data));
            /* Drain fd and ignore errors. */
            while (read (c_wokefd, zeroes, sizeof zeroes) == sizeof zeroes)
              ;
            break;
          }
      }
    }

  // Number of events triggered.
  result = data->rv;

  // Reset for next run loop.
  data->rv = 0;

  return scm_from_int (result);
}

#undef FUNC_NAME

void
init_libevt (void)
{
  time_units_per_microsec = scm_c_time_units_per_second / 1000000;

  scm_c_define_gsubr ("primitive-event-wake", 1, 0, 0,
                      scm_primitive_event_wake);
  scm_c_define_gsubr ("primitive-create-event-base", 1, 0, 0,
                      scm_primitive_create_event_base);
  scm_c_define_gsubr ("primitive-add-event", 3, 0, 0,
                      scm_primitive_add_event);
  scm_c_define_gsubr ("primitive-event-loop", 4, 0, 0,
                      scm_primitive_event_loop);

  scm_c_define ("%sizeof-struct-event",
                scm_from_size_t (sizeof (struct event_data)));
  scm_c_define ("%offsetof-struct-event-ev",
                scm_from_size_t (offsetof (struct event_data, event)));
  scm_c_define ("EVREAD", scm_from_int (EV_READ));
  scm_c_define ("EVWRITE", scm_from_int (EV_WRITE));
  scm_c_define ("EVPERSIST", scm_from_int (EV_PERSIST));
}

/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/