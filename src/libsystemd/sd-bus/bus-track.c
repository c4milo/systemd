/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "sd-bus.h"
#include "set.h"
#include "bus-internal.h"
#include "bus-track.h"

struct sd_bus_track {
        unsigned n_ref;
        sd_bus *bus;
        sd_bus_track_handler_t handler;
        void *userdata;
        Set *names;
        LIST_FIELDS(sd_bus_track, queue);
        Iterator iterator;
        bool in_queue;
        bool modified;
};

#define MATCH_PREFIX                                        \
        "type='signal',"                                    \
        "sender='org.freedesktop.DBus',"                    \
        "path='/org/freedesktop/DBus',"                     \
        "interface='org.freedesktop.DBus',"                 \
        "member='NameOwnerChanged',"                        \
        "arg0='"

#define MATCH_SUFFIX \
        "'"

#define MATCH_FOR_NAME(name)                                            \
        ({                                                              \
                char *_x;                                               \
                size_t _l = strlen(name);                               \
                _x = alloca(strlen(MATCH_PREFIX)+_l+strlen(MATCH_SUFFIX)+1); \
                strcpy(stpcpy(stpcpy(_x, MATCH_PREFIX), name), MATCH_SUFFIX); \
                _x;                                                     \
        })

static void bus_track_add_to_queue(sd_bus_track *track) {
        assert(track);

        if (track->in_queue)
                return;

        if (!track->handler)
                return;

        LIST_PREPEND(queue, track->bus->track_queue, track);
        track->in_queue = true;
}

static void bus_track_remove_from_queue(sd_bus_track *track) {
        assert(track);

        if (!track->in_queue)
                return;

        LIST_REMOVE(queue, track->bus->track_queue, track);
        track->in_queue = false;
}

_public_ int sd_bus_track_new(
                sd_bus *bus,
                sd_bus_track **track,
                sd_bus_track_handler_t handler,
                void *userdata) {

        sd_bus_track *t;

        assert_return(bus, -EINVAL);
        assert_return(track, -EINVAL);

        t = new0(sd_bus_track, 1);
        if (!t)
                return -ENOMEM;

        t->n_ref = 1;
        t->handler = handler;
        t->userdata = userdata;
        t->bus = sd_bus_ref(bus);

        bus_track_add_to_queue(t);

        *track = t;
        return 0;
}

_public_ sd_bus_track* sd_bus_track_ref(sd_bus_track *track) {
        assert_return(track, NULL);

        assert(track->n_ref > 0);

        track->n_ref++;

        return track;
}

_public_ sd_bus_track* sd_bus_track_unref(sd_bus_track *track) {
        const char *n;

        if (!track)
                return NULL;

        assert(track->n_ref > 0);

        if (track->n_ref > 1) {
                track->n_ref --;
                return NULL;
        }

        while ((n = set_first(track->names)))
                sd_bus_track_remove_name(track, n);

        bus_track_remove_from_queue(track);
        set_free(track->names);
        sd_bus_unref(track->bus);
        free(track);

        return NULL;
}

static int on_name_owner_changed(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        sd_bus_track *track = userdata;
        const char *name, *old, *new;
        int r;

        assert(bus);
        assert(message);
        assert(track);

        r = sd_bus_message_read(message, "sss", &name, &old, &new);
        if (r < 0)
                return 0;

        sd_bus_track_remove_name(track, name);
        return 0;
}

_public_ int sd_bus_track_add_name(sd_bus_track *track, const char *name) {
        _cleanup_free_ char *n = NULL;
        const char *match;
        int r;

        assert_return(track, -EINVAL);
        assert_return(service_name_is_valid(name), -EINVAL);

        r = set_ensure_allocated(&track->names, string_hash_func, string_compare_func);
        if (r < 0)
                return r;

        n = strdup(name);
        if (!n)
                return -ENOMEM;

        r = set_put(track->names, n);
        if (r == -EEXIST)
                return 0;
        if (r < 0)
                return r;

        /* First, subscribe to this name */
        match = MATCH_FOR_NAME(name);
        r = sd_bus_add_match(track->bus, match, on_name_owner_changed, track);
        if (r < 0) {
                set_remove(track->names, n);
                return r;
        }

        /* Second, check if it is currently existing, or maybe
         * doesn't, or maybe disappeared already. */
        r = sd_bus_get_owner(track->bus, name, 0, NULL);
        if (r < 0) {
                set_remove(track->names, n);
                sd_bus_remove_match(track->bus, match, on_name_owner_changed, track);
                return r;
        }

        n = NULL;

        bus_track_remove_from_queue(track);
        track->modified = true;

        return 1;
}

_public_ int sd_bus_track_remove_name(sd_bus_track *track, const char *name) {
        const char *match;
        _cleanup_free_ char *n = NULL;;

        assert_return(name, -EINVAL);

        if (!track)
                return 0;

        n = set_remove(track->names, (char*) name);
        if (!n)
                return 0;

        match = MATCH_FOR_NAME(n);
        sd_bus_remove_match(track->bus, match, on_name_owner_changed, track);

        if (set_isempty(track->names))
                bus_track_add_to_queue(track);

        track->modified = true;

        return 1;
}

_public_ unsigned sd_bus_track_count(sd_bus_track *track) {
        if (!track)
                return 0;

        return set_size(track->names);
}

_public_ const char* sd_bus_track_contains(sd_bus_track *track, const char *name) {
        assert_return(track, NULL);
        assert_return(name, NULL);

        return set_get(track->names, (void*) name);
}

_public_ const char* sd_bus_track_first(sd_bus_track *track) {
        if (!track)
                return NULL;

        track->modified = false;
        track->iterator = NULL;

        return set_iterate(track->names, &track->iterator);
}

_public_ const char* sd_bus_track_next(sd_bus_track *track) {
        if (!track)
                return NULL;

        if (track->modified)
                return NULL;

        return set_iterate(track->names, &track->iterator);
}

_public_ int sd_bus_track_add_sender(sd_bus_track *track, sd_bus_message *m) {
        const char *sender;

        assert_return(track, -EINVAL);
        assert_return(m, -EINVAL);

        sender = sd_bus_message_get_sender(m);
        if (!sender)
                return -EINVAL;

        return sd_bus_track_add_name(track, sender);
}

_public_ int sd_bus_track_remove_sender(sd_bus_track *track, sd_bus_message *m) {
        const char *sender;

        assert_return(track, -EINVAL);
        assert_return(m, -EINVAL);

        sender = sd_bus_message_get_sender(m);
        if (!sender)
                return -EINVAL;

        return sd_bus_track_remove_name(track, sender);
}

_public_ sd_bus* sd_bus_track_get_bus(sd_bus_track *track) {
        assert_return(track, NULL);

        return track->bus;
}

void bus_track_dispatch(sd_bus_track *track) {
        int r;

        assert(track);
        assert(track->in_queue);
        assert(track->handler);

        bus_track_remove_from_queue(track);

        sd_bus_track_ref(track);

        r = track->handler(track, track->userdata);
        if (r < 0)
                log_debug("Failed to process track handler: %s", strerror(-r));
        else if (r == 0)
                bus_track_add_to_queue(track);

        sd_bus_track_unref(track);
}