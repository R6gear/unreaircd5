/************************************************************************
 *   IRC - Internet Relay Chat, api-event.c
 *   (C) 2001- Carsten Munk (Techie/Stskeeps) <stskeeps@tspre.org>
 *   and the UnrealIRCd team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers. 
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

ID_Copyright("(C) Carsten Munk 2001");

MODVAR Event *events = NULL;

extern EVENT(unrealdns_removeoldrecords);
extern EVENT(unrealdb_expire_secret_cache);
extern EVENT(deprecated_notice);

/** Add an event, a function that will run at regular intervals.
 * @param module	Module that this event belongs to
 * @param name		Name of the event
 * @param event		The EVENT(function) to be called
 * @param data		The data to be passed to the function (or just NULL)
 * @param every_msec	Every <this> milliseconds the event will be called, but see notes.
 * @param count		After how many times we should stop calling this even (0 = infinite times)
 * @returns an Event struct
 * @note  UnrealIRCd will try to call the event every 'every_msec' milliseconds.
 *        However, in case of low traffic the minimum time is at least SOCKETLOOP_MAX_DELAY
 *        which is 250ms at the time of writing. Also, we reject any value below 100 msecs.
 *        The actual calling time will not be quicker than the specified every_msec but
 *        can be later, in case of high load, in very extreme cases even up to 1000 or 2000
 *        msec later but that would be very unusual. Just saying, it's not a guarantee..
 */
Event *EventAdd(Module *module, char *name, vFP event, void *data, long every_msec, int count)
{
	Event *newevent;

	if (!name || (every_msec < 0) || (count < 0) || !event)
	{
		if (module)
			module->errorcode = MODERR_INVALID;
		return NULL;
	}

	if ((every_msec < 100) && (count == 0))
	{
		ircd_log(LOG_ERROR, "[BUG] EventAdd() '%s' from module '%s' with suspiciously low every_msec value (%ld). "
		                    "Note that it is in milliseconds now (1000 = 1 second)!",
		                    name,
		                    module ? module->header->name : "???",
		                    every_msec);
		every_msec = 100;
	}

	newevent = safe_alloc(sizeof(Event));
	safe_strdup(newevent->name, name);
	newevent->count = count;
	newevent->every_msec = every_msec;
	newevent->event = event;
	newevent->data = data;
	newevent->last_run.tv_sec = timeofday_tv.tv_sec;
	newevent->last_run.tv_usec = timeofday_tv.tv_usec;
	newevent->owner = module;
	AddListItem(newevent,events);
	if (module)
	{
		ModuleObject *eventobj = safe_alloc(sizeof(ModuleObject));
		eventobj->object.event = newevent;
		eventobj->type = MOBJ_EVENT;
		AddListItem(eventobj, module->objects);
		module->errorcode = MODERR_NOERROR;
	}
	return newevent;
	
}

/** Mark the Event for deletion.
 * The actual deletion of the event happens later on
 * (which is of no concern to the caller).
 */
void EventDel(Event *e)
{
	char buf[128];

	/* Mark for deletion */
	e->deleted = 1;

	/* Replace the name so deleted events are clearly labeled */
	if (e->name)
	{
		snprintf(buf, sizeof(buf), "deleted:%s", e->name);
		safe_strdup(e->name, buf);
	}

	/* Remove the event from the module, that is something we can safely do straight away */
	if (e->owner)
	{
		ModuleObject *eventobjs;
		for (eventobjs = e->owner->objects; eventobjs; eventobjs = eventobjs->next)
		{
			if (eventobjs->type == MOBJ_EVENT && eventobjs->object.event == e)
			{
				DelListItem(eventobjs, e->owner->objects);
				safe_free(eventobjs);
				break;
			}
		}
		e->owner = NULL;
	}
}

/** Remove the event for real, used only via CleanupEvents(), not for end-users. */
static void EventDelReal(Event *e)
{
	if (!e->deleted)
	{
		ircd_log(LOG_ERROR, "EventDelReal called while e->deleted is 0. This cannot happen. Event name: %s.", e->name);
		abort();
	}
	if (e->owner)
	{
		ircd_log(LOG_ERROR, "EventDelReal called while e->owner is non-NULL. This cannot happen. Event name: %s.", e->name);
		abort();
	}
	safe_free(e->name);
	DelListItem(e, events);
	safe_free(e);
}

/** Remove any events that were previously marked for deletion */
static void CleanupEvents(void)
{
	Event *e, *e_next;
	for (e = events; e; e = e_next)
	{
		e_next = e->next;
		if (e->deleted)
			EventDelReal(e);
	}
}

Event *EventFind(char *name)
{
	Event *eventptr;

	for (eventptr = events; eventptr; eventptr = eventptr->next)
		if (!strcmp(eventptr->name, name))
			return (eventptr);
	return NULL;
}

int EventMod(Event *event, EventInfo *mods)
{
	if (!event || !mods)
	{
		if (event && event->owner)
			event->owner->errorcode = MODERR_INVALID;
		return -1;
	}

	if (mods->flags & EMOD_EVERY)
	{
		if (mods->every_msec < 100)
		{
			ircd_log(LOG_ERROR, "[BUG] EventMod() for '%s' from module '%s' with suspiciously low every_msec value (%lld). "
					    "Note that it is in milliseconds now (1000 = 1 second)!",
					    event->name,
					    event->owner ? event->owner->header->name : "???",
					    (long long)mods->every_msec);
			mods->every_msec = 100;
		}

		event->every_msec = mods->every_msec;
	}
	if (mods->flags & EMOD_HOWMANY)
		event->count = mods->count;
	if (mods->flags & EMOD_NAME)
		safe_strdup(event->name, mods->name);
	if (mods->flags & EMOD_EVENT)
		event->event = mods->event;
	if (mods->flags & EMOD_DATA)
		event->data = mods->data;
	if (event->owner)
		event->owner->errorcode = MODERR_NOERROR;
	return 0;
}

void DoEvents(void)
{
	Event *e;

	for (e = events; e; e = e->next)
	{
		if (e->deleted)
			continue;
		if (e->count == -1)
		{
			EventDel(e);
			continue;
		}
		if ((e->every_msec == 0) || minimum_msec_since_last_run(&e->last_run, e->every_msec))
		{
			(*e->event)(e->data);
			if (e->count > 0)
			{
				e->count--;
				if (e->count == 0)
				{
					EventDel(e);
					continue;
				}
			}
		}
	}

	CleanupEvents();
}

void SetupEvents(void)
{
	/* Start events */
	EventAdd(NULL, "tunefile", save_tunefile, NULL, 300*1000, 0);
	EventAdd(NULL, "garbage", garbage_collect, NULL, GARBAGE_COLLECT_EVERY*1000, 0);
	EventAdd(NULL, "loop", loop_event, NULL, 1000, 0);
	EventAdd(NULL, "unrealdns_removeoldrecords", unrealdns_removeoldrecords, NULL, 15000, 0);
	EventAdd(NULL, "deprecated_notice", deprecated_notice, NULL, ((86400*7)-(3600*8))*1000, 0);
	EventAdd(NULL, "check_pings", check_pings, NULL, 1000, 0);
	EventAdd(NULL, "check_deadsockets", check_deadsockets, NULL, 1000, 0);
	EventAdd(NULL, "handshake_timeout", handshake_timeout, NULL, 1000, 0);
	EventAdd(NULL, "tls_check_expiry", tls_check_expiry, NULL, (86400/2)*1000, 0);
	EventAdd(NULL, "unrealdb_expire_secret_cache", unrealdb_expire_secret_cache, NULL, 61000, 0);
	EventAdd(NULL, "throttling_check_expire", throttling_check_expire, NULL, 1000, 0);
}
