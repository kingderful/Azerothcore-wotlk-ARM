/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "CalendarMgr.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"

CalendarInvite::~CalendarInvite()
{
    // Free _inviteId only if it's a real invite and not just a pre-invite or guild announcement
    if (_inviteId != 0 && _eventId != 0)
        sCalendarMgr->FreeInviteId(_inviteId);
}

CalendarEvent::~CalendarEvent()
{
    sCalendarMgr->FreeEventId(_eventId);
}

CalendarMgr::CalendarMgr() : _maxEventId(0), _maxInviteId(0) { }

CalendarMgr::~CalendarMgr()
{
    for (CalendarEventStore::iterator itr = _events.begin(); itr != _events.end(); ++itr)
        delete *itr;

    for (CalendarEventInviteStore::iterator itr = _invites.begin(); itr != _invites.end(); ++itr)
        for (CalendarInviteStore::iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
            delete *itr2;
}

CalendarMgr* CalendarMgr::instance()
{
    static CalendarMgr instance;
    return &instance;
}

void CalendarMgr::LoadFromDB()
{
    uint32 count = 0;
    _maxEventId = 0;
    _maxInviteId = 0;

    //                                                       0   1        2      3            4     5        6          7      8
    if (QueryResult result = CharacterDatabase.Query("SELECT id, creator, title, description, type, dungeon, eventtime, flags, time2 FROM calendar_events"))
        do
        {
            Field* fields = result->Fetch();

            uint64 eventId          = fields[0].GetUInt64();
            uint64 creatorGUID      = MAKE_NEW_GUID(fields[1].GetUInt32(), 0, HIGHGUID_PLAYER);
            std::string title       = fields[2].GetString();
            std::string description = fields[3].GetString();
            CalendarEventType type  = CalendarEventType(fields[4].GetUInt8());
            int32 dungeonId         = fields[5].GetInt32();
            uint32 eventTime        = fields[6].GetUInt32();
            uint32 flags            = fields[7].GetUInt32();
            uint32 timezoneTime     = fields[8].GetUInt32();
            uint32 guildId = 0;

            if (flags & CALENDAR_FLAG_GUILD_EVENT || flags & CALENDAR_FLAG_WITHOUT_INVITES)
                guildId = Player::GetGuildIdFromStorage(GUID_LOPART(creatorGUID));

            CalendarEvent* calendarEvent = new CalendarEvent(eventId, creatorGUID, guildId, type, dungeonId, time_t(eventTime), flags, time_t(timezoneTime), title, description);
            _events.insert(calendarEvent);

            _maxEventId = std::max(_maxEventId, eventId);

            ++count;
        } while (result->NextRow());

    LOG_INFO("server", ">> Loaded %u calendar events", count);
    count = 0;

    //                                                       0   1      2        3       4       5            6      7
    if (QueryResult result = CharacterDatabase.Query("SELECT id, event, invitee, sender, status, statustime, `rank`, text FROM calendar_invites"))
        do
        {
            Field* fields = result->Fetch();

            uint64 inviteId             = fields[0].GetUInt64();
            uint64 eventId              = fields[1].GetUInt64();
            uint64 invitee              = MAKE_NEW_GUID(fields[2].GetUInt32(), 0, HIGHGUID_PLAYER);
            uint64 senderGUID           = MAKE_NEW_GUID(fields[3].GetUInt32(), 0, HIGHGUID_PLAYER);
            CalendarInviteStatus status = CalendarInviteStatus(fields[4].GetUInt8());
            uint32 statusTime           = fields[5].GetUInt32();
            CalendarModerationRank rank = CalendarModerationRank(fields[6].GetUInt8());
            std::string text            = fields[7].GetString();

            CalendarInvite* invite = new CalendarInvite(inviteId, eventId, invitee, senderGUID, time_t(statusTime), status, rank, text);
            _invites[eventId].push_back(invite);

            _maxInviteId = std::max(_maxInviteId, inviteId);

            ++count;
        } while (result->NextRow());

    LOG_INFO("server", ">> Loaded %u calendar invites", count);
    LOG_INFO("server", " ");

    for (uint64 i = 1; i < _maxEventId; ++i)
        if (!GetEvent(i))
            _freeEventIds.push_back(i);

    for (uint64 i = 1; i < _maxInviteId; ++i)
        if (!GetInvite(i))
            _freeInviteIds.push_back(i);
}

void CalendarMgr::AddEvent(CalendarEvent* calendarEvent, CalendarSendEventType sendType)
{
    _events.insert(calendarEvent);
    UpdateEvent(calendarEvent);
    SendCalendarEvent(calendarEvent->GetCreatorGUID(), *calendarEvent, sendType);
}

void CalendarMgr::AddInvite(CalendarEvent* calendarEvent, CalendarInvite* invite)
{
    SQLTransaction dummy;
    AddInvite(calendarEvent, invite, dummy);
}

void CalendarMgr::AddInvite(CalendarEvent* calendarEvent, CalendarInvite* invite, SQLTransaction& trans)
{
    if (!calendarEvent->IsGuildAnnouncement())
        SendCalendarEventInvite(*invite);

    if (!calendarEvent->IsGuildEvent() || invite->GetInviteeGUID() == calendarEvent->GetCreatorGUID())
        SendCalendarEventInviteAlert(*calendarEvent, *invite);

    if (!calendarEvent->IsGuildAnnouncement())
    {
        _invites[invite->GetEventId()].push_back(invite);
        UpdateInvite(invite, trans);
    }
}

void CalendarMgr::RemoveEvent(uint64 eventId, uint64 remover)
{
    CalendarEvent* calendarEvent = GetEvent(eventId);

    if (!calendarEvent)
    {
        SendCalendarCommandResult(remover, CALENDAR_ERROR_EVENT_INVALID);
        return;
    }

    RemoveEvent(calendarEvent, remover);
}

void CalendarMgr::RemoveEvent(CalendarEvent* calendarEvent, uint64 remover)
{
    if (!calendarEvent)
    {
        SendCalendarCommandResult(remover, CALENDAR_ERROR_EVENT_INVALID);
        return;
    }

    SendCalendarEventRemovedAlert(*calendarEvent);

    SQLTransaction trans = CharacterDatabase.BeginTransaction();
    PreparedStatement* stmt;
    MailDraft mail(calendarEvent->BuildCalendarMailSubject(remover), calendarEvent->BuildCalendarMailBody());

    CalendarInviteStore& eventInvites = _invites[calendarEvent->GetEventId()];
    for (size_t i = 0; i < eventInvites.size(); ++i)
    {
        CalendarInvite* invite = eventInvites[i];
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CALENDAR_INVITE);
        stmt->setUInt64(0, invite->GetInviteId());
        trans->Append(stmt);

        // guild events only? check invite status here?
        // When an event is deleted, all invited (accepted/declined? - verify) guildies are notified via in-game mail. (wowwiki)
        if (remover && invite->GetInviteeGUID() != remover)
            mail.SendMailTo(trans, MailReceiver(invite->GetInviteeGUID()), calendarEvent, MAIL_CHECK_MASK_COPIED);

        delete invite;
    }

    _invites.erase(calendarEvent->GetEventId());

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CALENDAR_EVENT);
    stmt->setUInt64(0, calendarEvent->GetEventId());
    trans->Append(stmt);
    CharacterDatabase.CommitTransaction(trans);

    delete calendarEvent;
    _events.erase(calendarEvent);
    return;
}

void CalendarMgr::RemoveInvite(uint64 inviteId, uint64 eventId, uint64 /*remover*/)
{
    CalendarEvent* calendarEvent = GetEvent(eventId);

    if (!calendarEvent)
        return;

    CalendarInviteStore::iterator itr = _invites[eventId].begin();
    for (; itr != _invites[eventId].end(); ++itr)
        if ((*itr)->GetInviteId() == inviteId)
            break;

    if (itr == _invites[eventId].end())
        return;

    SQLTransaction trans = CharacterDatabase.BeginTransaction();
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CALENDAR_INVITE);
    stmt->setUInt64(0, (*itr)->GetInviteId());
    trans->Append(stmt);
    CharacterDatabase.CommitTransaction(trans);

    if (!calendarEvent->IsGuildEvent())
        SendCalendarEventInviteRemoveAlert((*itr)->GetInviteeGUID(), *calendarEvent, CALENDAR_STATUS_REMOVED);

    SendCalendarEventInviteRemove(*calendarEvent, **itr, calendarEvent->GetFlags());

    // we need to find out how to use CALENDAR_INVITE_REMOVED_MAIL_SUBJECT to force client to display different mail
    //if ((*itr)->GetInviteeGUID() != remover)
    //    MailDraft(calendarEvent->BuildCalendarMailSubject(remover), calendarEvent->BuildCalendarMailBody())
    //        .SendMailTo(trans, MailReceiver((*itr)->GetInvitee()), calendarEvent, MAIL_CHECK_MASK_COPIED);

    delete *itr;
    _invites[eventId].erase(itr);
}

void CalendarMgr::UpdateEvent(CalendarEvent* calendarEvent)
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_CALENDAR_EVENT);
    stmt->setUInt64(0, calendarEvent->GetEventId());
    stmt->setUInt32(1, GUID_LOPART(calendarEvent->GetCreatorGUID()));
    stmt->setString(2, calendarEvent->GetTitle());
    stmt->setString(3, calendarEvent->GetDescription());
    stmt->setUInt8(4, calendarEvent->GetType());
    stmt->setInt32(5, calendarEvent->GetDungeonId());
    stmt->setUInt32(6, uint32(calendarEvent->GetEventTime()));
    stmt->setUInt32(7, calendarEvent->GetFlags());
    stmt->setUInt32(8, calendarEvent->GetTimeZoneTime()); // correct?
    CharacterDatabase.Execute(stmt);
}

void CalendarMgr::UpdateInvite(CalendarInvite* invite)
{
    SQLTransaction dummy;
    UpdateInvite(invite, dummy);
}

void CalendarMgr::UpdateInvite(CalendarInvite* invite, SQLTransaction& trans)
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_CALENDAR_INVITE);
    stmt->setUInt64(0, invite->GetInviteId());
    stmt->setUInt64(1, invite->GetEventId());
    stmt->setUInt32(2, GUID_LOPART(invite->GetInviteeGUID()));
    stmt->setUInt32(3, GUID_LOPART(invite->GetSenderGUID()));
    stmt->setUInt8(4, invite->GetStatus());
    stmt->setUInt32(5, uint32(invite->GetStatusTime()));
    stmt->setUInt8(6, invite->GetRank());
    stmt->setString(7, invite->GetText());
    CharacterDatabase.ExecuteOrAppend(trans, stmt);
}

void CalendarMgr::RemoveAllPlayerEventsAndInvites(uint64 guid)
{
    for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end();)
    {
        CalendarEvent* event = *itr;
        ++itr;
        if (event->GetCreatorGUID() == guid)
        {
            RemoveEvent(event, 0);
            continue;
        }
    }

    CalendarInviteStore playerInvites = GetPlayerInvites(guid);
    for (CalendarInviteStore::const_iterator itr = playerInvites.begin(); itr != playerInvites.end(); ++itr)
        RemoveInvite((*itr)->GetInviteId(), (*itr)->GetEventId(), guid);
}

void CalendarMgr::RemovePlayerGuildEventsAndSignups(uint64 guid, uint32 guildId)
{
    for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end(); ++itr)
        if ((*itr)->GetCreatorGUID() == guid && ((*itr)->IsGuildEvent() || (*itr)->IsGuildAnnouncement()))
            RemoveEvent((*itr)->GetEventId(), guid);

    CalendarInviteStore playerInvites = GetPlayerInvites(guid);
    for (CalendarInviteStore::const_iterator itr = playerInvites.begin(); itr != playerInvites.end(); ++itr)
        if (CalendarEvent* calendarEvent = GetEvent((*itr)->GetEventId()))
            if (calendarEvent->IsGuildEvent() && calendarEvent->GetGuildId() == guildId)
                RemoveInvite((*itr)->GetInviteId(), (*itr)->GetEventId(), guid);
}

CalendarEvent* CalendarMgr::GetEvent(uint64 eventId)
{
    for (CalendarEventStore::iterator itr = _events.begin(); itr != _events.end(); ++itr)
        if ((*itr)->GetEventId() == eventId)
            return *itr;

    return nullptr;
}

CalendarInvite* CalendarMgr::GetInvite(uint64 inviteId) const
{
    for (CalendarEventInviteStore::const_iterator itr = _invites.begin(); itr != _invites.end(); ++itr)
        for (CalendarInviteStore::const_iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
            if ((*itr2)->GetInviteId() == inviteId)
                return *itr2;

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
    LOG_DEBUG("entities.unit", "CalendarMgr::GetInvite: [" UI64FMTD "] not found!", inviteId);
#endif
    return nullptr;
}

void CalendarMgr::FreeEventId(uint64 id)
{
    if (id == _maxEventId)
        --_maxEventId;
    else
        _freeEventIds.push_back(id);
}

uint64 CalendarMgr::GetFreeEventId()
{
    if (_freeEventIds.empty())
        return ++_maxEventId;

    uint64 eventId = _freeEventIds.front();
    _freeEventIds.pop_front();
    return eventId;
}

void CalendarMgr::FreeInviteId(uint64 id)
{
    if (id == _maxInviteId)
        --_maxInviteId;
    else
        _freeInviteIds.push_back(id);
}

uint64 CalendarMgr::GetFreeInviteId()
{
    if (_freeInviteIds.empty())
        return ++_maxInviteId;

    uint64 inviteId = _freeInviteIds.front();
    _freeInviteIds.pop_front();
    return inviteId;
}

void CalendarMgr::DeleteOldEvents()
{
    time_t oldEventsTime = time(nullptr) - CALENDAR_OLD_EVENTS_DELETION_TIME;

    for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end();)
    {
        CalendarEvent* event = *itr;
        ++itr;
        if (event->GetEventTime() < oldEventsTime)
            RemoveEvent(event, 0);
    }
}

CalendarEventStore CalendarMgr::GetEventsCreatedBy(uint64 guid, bool includeGuildEvents)
{
    CalendarEventStore result;
    for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end(); ++itr)
        if ((*itr)->GetCreatorGUID() == guid && (includeGuildEvents || (!(*itr)->IsGuildEvent() && !(*itr)->IsGuildAnnouncement())))
            result.insert(*itr);

    return result;
}

CalendarEventStore CalendarMgr::GetGuildEvents(uint32 guildId)
{
    CalendarEventStore result;

    if (!guildId)
        return result;

    for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end(); ++itr)
        if ((*itr)->IsGuildEvent() || (*itr)->IsGuildAnnouncement())
            if ((*itr)->GetGuildId() == guildId)
                result.insert(*itr);

    return result;
}

CalendarEventStore CalendarMgr::GetPlayerEvents(uint64 guid)
{
    CalendarEventStore events;

    for (CalendarEventInviteStore::const_iterator itr = _invites.begin(); itr != _invites.end(); ++itr)
        for (CalendarInviteStore::const_iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
            if ((*itr2)->GetInviteeGUID() == guid)
                if (CalendarEvent* event = GetEvent(itr->first)) // nullptr check added as attempt to fix #11512
                    events.insert(event);

    if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(guid))
        if (player->GetGuildId())
            for (CalendarEventStore::const_iterator itr = _events.begin(); itr != _events.end(); ++itr)
                if ((*itr)->GetGuildId() == player->GetGuildId())
                    events.insert(*itr);

    return events;
}

CalendarInviteStore const& CalendarMgr::GetEventInvites(uint64 eventId)
{
    return _invites[eventId];
}

CalendarInviteStore CalendarMgr::GetPlayerInvites(uint64 guid)
{
    CalendarInviteStore invites;

    for (CalendarEventInviteStore::const_iterator itr = _invites.begin(); itr != _invites.end(); ++itr)
        for (CalendarInviteStore::const_iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
            if ((*itr2)->GetInviteeGUID() == guid)
                invites.push_back(*itr2);

    return invites;
}

uint32 CalendarMgr::GetPlayerNumPending(uint64 guid)
{
    CalendarInviteStore const& invites = GetPlayerInvites(guid);

    uint32 pendingNum = 0;
    for (CalendarInviteStore::const_iterator itr = invites.begin(); itr != invites.end(); ++itr)
    {
        switch ((*itr)->GetStatus())
        {
            case CALENDAR_STATUS_INVITED:
            case CALENDAR_STATUS_TENTATIVE:
            case CALENDAR_STATUS_NOT_SIGNED_UP:
                ++pendingNum;
                break;
            default:
                break;
        }
    }

    return pendingNum;
}

std::string CalendarEvent::BuildCalendarMailSubject(uint64 remover) const
{
    std::ostringstream strm;
    strm << remover << ':' << _title;
    return strm.str();
}

std::string CalendarEvent::BuildCalendarMailBody() const
{
    WorldPacket data;
    uint32 time;
    std::ostringstream strm;

    // we are supposed to send PackedTime so i used WorldPacket to pack it
    data.AppendPackedTime(_eventTime);
    data >> time;
    strm << time;
    return strm.str();
}

void CalendarMgr::SendCalendarEventInvite(CalendarInvite const& invite)
{
    CalendarEvent* calendarEvent = GetEvent(invite.GetEventId());
    time_t statusTime = invite.GetStatusTime();
    bool hasStatusTime = statusTime != 946684800;   // 01/01/2000 00:00:00

    uint64 invitee = invite.GetInviteeGUID();
    Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(invitee);

    uint8 level = player ? player->getLevel() : Player::GetLevelFromStorage(invitee);

    WorldPacket data(SMSG_CALENDAR_EVENT_INVITE, 8 + 8 + 8 + 1 + 1 + 1 + (statusTime ? 4 : 0) + 1);
    data.appendPackGUID(invitee);
    data << uint64(invite.GetEventId());
    data << uint64(invite.GetInviteId());
    data << uint8(level);
    data << uint8(invite.GetStatus());
    data << uint8(hasStatusTime);
    if (hasStatusTime)
        data.AppendPackedTime(statusTime);
    data << uint8(invite.GetSenderGUID() != invite.GetInviteeGUID()); // false only if the invite is sign-up

    if (!calendarEvent) // Pre-invite
    {
        if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(invite.GetSenderGUID()))
            player->SendDirectMessage(&data);
    }
    else
    {
        if (calendarEvent->GetCreatorGUID() != invite.GetInviteeGUID()) // correct?
            SendPacketToAllEventRelatives(data, *calendarEvent);
    }
}

void CalendarMgr::SendCalendarEventUpdateAlert(CalendarEvent const& calendarEvent, time_t oldEventTime)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_UPDATED_ALERT, 1 + 8 + 4 + 4 + 4 + 1 + 4 +
                     calendarEvent.GetTitle().size() + calendarEvent.GetDescription().size() + 1 + 4 + 4);
    data << uint8(1);       // unk
    data << uint64(calendarEvent.GetEventId());
    data.AppendPackedTime(oldEventTime);
    data << uint32(calendarEvent.GetFlags());
    data.AppendPackedTime(calendarEvent.GetEventTime());
    data << uint8(calendarEvent.GetType());
    data << int32(calendarEvent.GetDungeonId());
    data << calendarEvent.GetTitle();
    data << calendarEvent.GetDescription();
    data << uint8(CALENDAR_REPEAT_NEVER);   // repeatable
    data << uint32(CALENDAR_MAX_INVITES);
    data << uint32(0);      // unk

    SendPacketToAllEventRelatives(data, calendarEvent);
}

void CalendarMgr::SendCalendarEventStatus(CalendarEvent const& calendarEvent, CalendarInvite const& invite)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_STATUS, 8 + 8 + 4 + 4 + 1 + 1 + 4);
    data.appendPackGUID(invite.GetInviteeGUID());
    data << uint64(calendarEvent.GetEventId());
    data.AppendPackedTime(calendarEvent.GetEventTime());
    data << uint32(calendarEvent.GetFlags());
    data << uint8(invite.GetStatus());
    data << uint8(invite.GetRank());
    data.AppendPackedTime(invite.GetStatusTime());

    SendPacketToAllEventRelatives(data, calendarEvent);
}

void CalendarMgr::SendCalendarEventRemovedAlert(CalendarEvent const& calendarEvent)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_REMOVED_ALERT, 1 + 8 + 1);
    data << uint8(1); // FIXME: If true does not SignalEvent(EVENT_CALENDAR_ACTION_PENDING)
    data << uint64(calendarEvent.GetEventId());
    data.AppendPackedTime(calendarEvent.GetEventTime());

    SendPacketToAllEventRelatives(data, calendarEvent);
}

void CalendarMgr::SendCalendarEventInviteRemove(CalendarEvent const& calendarEvent, CalendarInvite const& invite, uint32 flags)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_INVITE_REMOVED, 8 + 4 + 4 + 1);
    data.appendPackGUID(invite.GetInviteeGUID());
    data << uint64(invite.GetEventId());
    data << uint32(flags);
    data << uint8(1); // FIXME

    SendPacketToAllEventRelatives(data, calendarEvent);
}

void CalendarMgr::SendCalendarEventModeratorStatusAlert(CalendarEvent const& calendarEvent, CalendarInvite const& invite)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT, 8 + 8 + 1 + 1);
    data.appendPackGUID(invite.GetInviteeGUID());
    data << uint64(invite.GetEventId());
    data << uint8(invite.GetRank());
    data << uint8(1); // Unk boolean - Display to client?

    SendPacketToAllEventRelatives(data, calendarEvent);
}

void CalendarMgr::SendCalendarEventInviteAlert(CalendarEvent const& calendarEvent, CalendarInvite const& invite)
{
    WorldPacket data(SMSG_CALENDAR_EVENT_INVITE_ALERT);
    data << uint64(calendarEvent.GetEventId());
    data << calendarEvent.GetTitle();
    data.AppendPackedTime(calendarEvent.GetEventTime());
    data << uint32(calendarEvent.GetFlags());
    data << uint32(calendarEvent.GetType());
    data << int32(calendarEvent.GetDungeonId());
    data << uint64(invite.GetInviteId());
    data << uint8(invite.GetStatus());
    data << uint8(invite.GetRank());
    data.appendPackGUID(calendarEvent.GetCreatorGUID());
    data.appendPackGUID(invite.GetSenderGUID());

    if (calendarEvent.IsGuildEvent() || calendarEvent.IsGuildAnnouncement())
    {
        if (Guild* guild = sGuildMgr->GetGuildById(calendarEvent.GetGuildId()))
            guild->BroadcastPacket(&data);
    }
    else if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(invite.GetInviteeGUID()))
        player->SendDirectMessage(&data);
}

void CalendarMgr::SendCalendarEvent(uint64 guid, CalendarEvent const& calendarEvent, CalendarSendEventType sendType)
{
    Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(guid);
    if (!player)
        return;

    CalendarInviteStore const& eventInviteeList = _invites[calendarEvent.GetEventId()];

    WorldPacket data(SMSG_CALENDAR_SEND_EVENT, 60 + eventInviteeList.size() * 32);
    data << uint8(sendType);
    data.appendPackGUID(calendarEvent.GetCreatorGUID());
    data << uint64(calendarEvent.GetEventId());
    data << calendarEvent.GetTitle();
    data << calendarEvent.GetDescription();
    data << uint8(calendarEvent.GetType());
    data << uint8(CALENDAR_REPEAT_NEVER);   // repeatable
    data << uint32(CALENDAR_MAX_INVITES);
    data << int32(calendarEvent.GetDungeonId());
    data << uint32(calendarEvent.GetFlags());
    data.AppendPackedTime(calendarEvent.GetEventTime());
    data.AppendPackedTime(calendarEvent.GetTimeZoneTime());
    data << uint32(calendarEvent.GetGuildId());

    data << uint32(eventInviteeList.size());
    for (CalendarInviteStore::const_iterator itr = eventInviteeList.begin(); itr != eventInviteeList.end(); ++itr)
    {
        CalendarInvite const* calendarInvite = (*itr);
        uint64 inviteeGuid = calendarInvite->GetInviteeGUID();
        Player* invitee = ObjectAccessor::FindPlayerInOrOutOfWorld(inviteeGuid);

        uint8 inviteeLevel = invitee ? invitee->getLevel() : Player::GetLevelFromStorage(inviteeGuid);
        uint32 inviteeGuildId = invitee ? invitee->GetGuildId() : Player::GetGuildIdFromStorage(GUID_LOPART(inviteeGuid));

        data.appendPackGUID(inviteeGuid);
        data << uint8(inviteeLevel);
        data << uint8(calendarInvite->GetStatus());
        data << uint8(calendarInvite->GetRank());
        data << uint8(calendarEvent.IsGuildEvent() && calendarEvent.GetGuildId() == inviteeGuildId);
        data << uint64(calendarInvite->GetInviteId());
        data.AppendPackedTime(calendarInvite->GetStatusTime());
        data << calendarInvite->GetText();
    }

    player->SendDirectMessage(&data);
}

void CalendarMgr::SendCalendarEventInviteRemoveAlert(uint64 guid, CalendarEvent const& calendarEvent, CalendarInviteStatus status)
{
    if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(guid))
    {
        WorldPacket data(SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT, 8 + 4 + 4 + 1);
        data << uint64(calendarEvent.GetEventId());
        data.AppendPackedTime(calendarEvent.GetEventTime());
        data << uint32(calendarEvent.GetFlags());
        data << uint8(status);

        player->SendDirectMessage(&data);
    }
}

void CalendarMgr::SendCalendarClearPendingAction(uint64 guid)
{
    if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(guid))
    {
        WorldPacket data(SMSG_CALENDAR_CLEAR_PENDING_ACTION, 0);
        player->SendDirectMessage(&data);
    }
}

void CalendarMgr::SendCalendarCommandResult(uint64 guid, CalendarError err, char const* param /*= nullptr*/)
{
    if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld(guid))
    {
        WorldPacket data(SMSG_CALENDAR_COMMAND_RESULT, 0);
        data << uint32(0);
        data << uint8(0);
        switch (err)
        {
            case CALENDAR_ERROR_OTHER_INVITES_EXCEEDED:
            case CALENDAR_ERROR_ALREADY_INVITED_TO_EVENT_S:
            case CALENDAR_ERROR_IGNORING_YOU_S:
                data << param;
                break;
            default:
                data << uint8(0);
                break;
        }

        data << uint32(err);

        player->SendDirectMessage(&data);
    }
}

void CalendarMgr::SendPacketToAllEventRelatives(WorldPacket packet, CalendarEvent const& calendarEvent)
{
    // Send packet to all guild members
    if (calendarEvent.IsGuildEvent() || calendarEvent.IsGuildAnnouncement())
        if (Guild* guild = sGuildMgr->GetGuildById(calendarEvent.GetGuildId()))
            guild->BroadcastPacket(&packet);

    // Send packet to all invitees if event is non-guild, in other case only to non-guild invitees (packet was broadcasted for them)
    CalendarInviteStore invites = _invites[calendarEvent.GetEventId()];
    for (CalendarInviteStore::iterator itr = invites.begin(); itr != invites.end(); ++itr)
        if (Player* player = ObjectAccessor::FindPlayerInOrOutOfWorld((*itr)->GetInviteeGUID()))
            if (!calendarEvent.IsGuildEvent() || player->GetGuildId() != calendarEvent.GetGuildId())
                player->SendDirectMessage(&packet);
}
