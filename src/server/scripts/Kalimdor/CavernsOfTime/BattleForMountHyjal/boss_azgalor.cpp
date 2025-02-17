/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "hyjal_trash.h"
#include "hyjal.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"

enum Spells
{
    SPELL_RAIN_OF_FIRE          = 31340,
    SPELL_DOOM                  = 31347,
    SPELL_HOWL_OF_AZGALOR       = 31344,
    SPELL_CLEAVE                = 31345,
    SPELL_BERSERK               = 26662,

    SPELL_THRASH                = 12787,
    SPELL_CRIPPLE               = 31406,
    SPELL_WARSTOMP              = 31408,
};

enum Texts
{
    SAY_ONDEATH             = 0,
    SAY_ONSLAY              = 1,
    SAY_DOOM                = 2, // Not used?
    SAY_ONAGGRO             = 3,
};

class boss_azgalor : public CreatureScript
{
public:
    boss_azgalor() : CreatureScript("boss_azgalor") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetHyjalAI<boss_azgalorAI>(creature);
    }

    struct boss_azgalorAI : public hyjal_trashAI
    {
        boss_azgalorAI(Creature* creature) : hyjal_trashAI(creature)
        {
            instance = creature->GetInstanceScript();
            go = false;
        }

        uint32 RainTimer;
        uint32 DoomTimer;
        uint32 HowlTimer;
        uint32 CleaveTimer;
        uint32 EnrageTimer;
        bool enraged;

        bool go;

        void Reset() override
        {
            damageTaken = 0;
            RainTimer = 20000;
            DoomTimer = 50000;
            HowlTimer = 30000;
            CleaveTimer = 10000;
            EnrageTimer = 600000;
            enraged = false;

            if (IsEvent)
                instance->SetData(DATA_AZGALOREVENT, NOT_STARTED);
        }

        void EnterCombat(Unit* /*who*/) override
        {
            if (IsEvent)
                instance->SetData(DATA_AZGALOREVENT, IN_PROGRESS);

            Talk(SAY_ONAGGRO);
        }

        void KilledUnit(Unit* /*victim*/) override
        {
            Talk(SAY_ONSLAY);
        }

        void WaypointReached(uint32 waypointId) override
        {
            if (waypointId == 7 && instance)
            {
                Unit* target = ObjectAccessor::GetUnit(*me, instance->GetData64(DATA_THRALL));
                if (target && target->IsAlive())
                    me->AddThreat(target, 0.0f);
            }
        }

        void JustDied(Unit* killer) override
        {
            hyjal_trashAI::JustDied(killer);
            if (IsEvent)
                instance->SetData(DATA_AZGALOREVENT, DONE);
            Talk(SAY_ONDEATH);
        }

        void UpdateAI(uint32 diff) override
        {
            if (IsEvent)
            {
                //Must update npc_escortAI
                npc_escortAI::UpdateAI(diff);
                if (!go)
                {
                    go = true;
                    AddWaypoint(0, 5492.91f,    -2404.61f,    1462.63f);
                    AddWaypoint(1, 5531.76f,    -2460.87f,    1469.55f);
                    AddWaypoint(2, 5554.58f,    -2514.66f,    1476.12f);
                    AddWaypoint(3, 5554.16f,    -2567.23f,    1479.90f);
                    AddWaypoint(4, 5540.67f,    -2625.99f,    1480.89f);
                    AddWaypoint(5, 5508.16f,    -2659.2f,    1480.15f);
                    AddWaypoint(6, 5489.62f,    -2704.05f,    1482.18f);
                    AddWaypoint(7, 5457.04f,    -2726.26f,    1485.10f);
                    Start(false, true);
                    SetDespawnAtEnd(false);
                }
            }

            //Return since we have no target
            if (!UpdateVictim())
                return;

            if (RainTimer <= diff)
            {
                DoCast(SelectTarget(SELECT_TARGET_RANDOM, 0, 30, true), SPELL_RAIN_OF_FIRE);
                RainTimer = 20000 + rand() % 15000;
            }
            else RainTimer -= diff;

            if (DoomTimer <= diff)
            {
                DoCast(SelectTarget(SELECT_TARGET_RANDOM, 1, 100, true), SPELL_DOOM);//never on tank
                DoomTimer = 45000 + rand() % 5000;
            }
            else DoomTimer -= diff;

            if (HowlTimer <= diff)
            {
                DoCast(me, SPELL_HOWL_OF_AZGALOR);
                HowlTimer = 30000;
            }
            else HowlTimer -= diff;

            if (CleaveTimer <= diff)
            {
                DoCastVictim(SPELL_CLEAVE);
                CleaveTimer = 10000 + rand() % 5000;
            }
            else CleaveTimer -= diff;

            if (EnrageTimer < diff && !enraged)
            {
                me->InterruptNonMeleeSpells(false);
                DoCast(me, SPELL_BERSERK, true);
                enraged = true;
                EnrageTimer = 600000;
            }
            else EnrageTimer -= diff;

            DoMeleeAttackIfReady();
        }
    };
};

class npc_lesser_doomguard : public CreatureScript
{
public:
    npc_lesser_doomguard() : CreatureScript("npc_lesser_doomguard") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return GetHyjalAI<npc_lesser_doomguardAI>(creature);
    }

    struct npc_lesser_doomguardAI : public hyjal_trashAI
    {
        npc_lesser_doomguardAI(Creature* creature) : hyjal_trashAI(creature)
        {
            instance = creature->GetInstanceScript();
            AzgalorGUID = instance->GetData64(DATA_AZGALOR);
        }

        uint32 CrippleTimer;
        uint32 WarstompTimer;
        uint32 CheckTimer;
        uint64 AzgalorGUID;
        InstanceScript* instance;

        void Reset() override
        {
            CrippleTimer = 50000;
            WarstompTimer = 10000;
            DoCast(me, SPELL_THRASH);
            CheckTimer = 5000;
        }

        void EnterCombat(Unit* /*who*/) override
        {
        }

        void KilledUnit(Unit* /*victim*/) override
        {
        }

        void WaypointReached(uint32 /*waypointId*/) override
        {
        }

        void MoveInLineOfSight(Unit* who) override

        {
            if (me->IsWithinDist(who, 50) && !me->IsInCombat() && me->IsValidAttackTarget(who))
                AttackStart(who);
        }

        void JustDied(Unit* /*killer*/) override
        {
        }

        void UpdateAI(uint32 diff) override
        {
            if (CheckTimer <= diff)
            {
                if (AzgalorGUID)
                {
                    Creature* boss = ObjectAccessor::GetCreature(*me, AzgalorGUID);
                    if (!boss || (boss && boss->isDead()))
                    {
                        me->setDeathState(JUST_DIED);
                        me->RemoveCorpse();
                        return;
                    }
                }
                CheckTimer = 5000;
            }
            else CheckTimer -= diff;

            //Return since we have no target
            if (!UpdateVictim())
                return;

            if (WarstompTimer <= diff)
            {
                DoCast(me, SPELL_WARSTOMP);
                WarstompTimer = 10000 + rand() % 5000;
            }
            else WarstompTimer -= diff;

            if (CrippleTimer <= diff)
            {
                DoCast(SelectTarget(SELECT_TARGET_RANDOM, 0, 100, true), SPELL_CRIPPLE);
                CrippleTimer = 25000 + rand() % 5000;
            }
            else CrippleTimer -= diff;

            DoMeleeAttackIfReady();
        }
    };
};

void AddSC_boss_azgalor()
{
    new boss_azgalor();
    new npc_lesser_doomguard();
}
