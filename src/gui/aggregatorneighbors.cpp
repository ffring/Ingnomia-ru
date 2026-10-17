/*	
	This file is part of Ingnomia https://github.com/rschurade/Ingnomia
    Copyright (C) 2017-2020  Ralph Schurade, Ingnomia Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
/** @file aggregatorneighbors.cpp
 *  @brief AggregatorNeighbors implementation: produces human-readable descriptions of each
 *         neighbouring kingdom (distance, wealth, economy, military, attitude), lists
 *         available gnomes for diplomacy missions, and forwards mission start/update events.
 */
#include "aggregatorneighbors.h"

#include "../game/game.h"
#include "../game/neighbormanager.h"
#include "../game/gnomemanager.h"

#include "../gui/strings.h"

/// @brief Constructs the AggregatorNeighbors.
/// @param parent Qt parent object.
AggregatorNeighbors::AggregatorNeighbors( QObject* parent ) :
	QObject(parent)
{

}

/// @brief Destructor.
AggregatorNeighbors::~AggregatorNeighbors()
{
}

/// @brief Binds the aggregator to a Game instance.
/// @param game Game to bind to.
void AggregatorNeighbors::init( Game* game )
{
	g = game;
}

/// @brief Rebuilds the cached neighbour list from NeighborManager::kingdoms(), composing
///        flavour-text descriptions for type, distance, wealth, economy, military, and
///        diplomatic attitude. Emits signalNeighborsUpdate when done.
void AggregatorNeighbors::onRequestNeighborsUpdate()
{
	if( !g ) return;
	m_neighborsInfo.clear();

	for( const auto& kingdom : g->nm()->kingdoms() )
	{
		GuiNeighborInfo gni;

		gni.id = kingdom.id;

		//if( kingdom.discovered || Global::debugMode )
		{
			gni.discovered = true;

			QString name = kingdom.name + " — ";
			switch( kingdom.type )
			{
				case KingdomType::GNOME:
				name += "королевство гномов";
				gni.diploMission = true;
				break;
			case KingdomType::GOBLIN:
				name += "королевство гоблинов";

				gni.spyMission = true;
				gni.sabotageMission = true;
				gni.raidMission = true;
				gni.diploMission = true;

				break;
			default:
				name += "кто они, мы не знаем";
				break;
			}

			gni.name = name;

			const int days = kingdom.distance / 24;
			gni.distance = "Дорога туда займёт около " + QString::number( days ) + " " + S::plural( days, "дня", "дней", "дней" ) + ".";

			QString wealth = "Достаток: ";
			switch ( kingdom.wealth )
			{
				case KingdomWealth::VERYPOOR:
				wealth += "очень бедны.";
				break;
			case KingdomWealth::POOR:
				wealth += "бедны.";
				break;
			case KingdomWealth::AVERAGE:
				wealth += "живут неплохо.";
				break;
			case KingdomWealth::RICH:
				wealth += "богаты.";
				break;
			case KingdomWealth::VERYRICH:
				wealth += "очень богаты.";
				break;
			}
			gni.wealth = wealth;

			QString economy = "Основное занятие: ";
			switch (kingdom.economy )
			{
				case KingdomEconomy::FARMING:
				economy += "земледелие.";
				break;
			case KingdomEconomy::LOGGING:
				economy += "лесозаготовка.";
				break;
			case KingdomEconomy::MINING:
				economy += "горное дело.";
				break;
			case KingdomEconomy::ANIMALBREEDING:
				economy += "животноводство.";
				break;
			case KingdomEconomy::TRADING:
				economy += "торговля.";
				break;
			}
			gni.economy = economy;

			QString military = "Армия: ";
			switch ( kingdom.military )
			{
				case KingdomMilitary::VERYWEAK:
				military += "очень слабая.";
				break;
			case KingdomMilitary::WEAK:
				military += "слабая.";
				break;
			case KingdomMilitary::AVERAGE:
				military += "средняя.";
				break;
			case KingdomMilitary::STRONG:
				military += "сильная.";
				break;
			case KingdomMilitary::VERYSTRONG:
				military += "очень сильная.";
				break;
			}
			gni.military = military;

			QString attitude = "Относятся к нам нейтрально.";
			if( kingdom.attitude < -75 )
			{
				attitude = "Считают нас врагами.";
			}
			else if( kingdom.attitude < -50  )
			{
				attitude = "Настроены очень враждебно.";
			}
			else if( kingdom.attitude < -25 )
			{
				attitude = "Настроены враждебно.";
			}
			else if( kingdom.attitude > 25 )
			{
				attitude = "Настроены дружелюбно.";
			}
			else if( kingdom.attitude > 50 )
			{
				attitude = "Настроены очень дружелюбно.";
			}
			else if( kingdom.attitude > 75 )
			{
				attitude = "Считают нас друзьями.";
			}
			gni.attitude = attitude;
		}

		m_neighborsInfo.append( gni );
	}

	emit signalNeighborsUpdate( m_neighborsInfo );
}

/// @brief Emits the current list of active missions from EventManager.
void AggregatorNeighbors::onRequestMissions()
{
	if( !g ) return;
	emit signalMissions( g->em()->missions() );
}

/// @brief Rebuilds the list of gnomes not currently on any mission and emits signalAvailableGnomes.
void AggregatorNeighbors::onRequestAvailableGnomes()
{
	if( !g ) return;
	m_availableGnomes.clear();

	for( auto gnome : g->gm()->gnomes() )
	{
		if( !gnome->isOnMission() )
		{
			m_availableGnomes.append( { gnome->id(), gnome->name() } );
		}
	}

	emit signalAvailableGnomes( m_availableGnomes );
}

/// @brief Starts a new mission of @p type against @p targetKingdom with the given gnome and
///        emits a fresh mission list to the GUI.
/// @param type          Mission type (spy/sabotage/raid/emissary).
/// @param action        Specific action within that mission type.
/// @param targetKingdom Target kingdom UID.
/// @param gnomeID       Creature UID of the dispatched gnome.
void AggregatorNeighbors::onStartMission( MissionType type, MissionAction action, unsigned int targetKingdom, unsigned int gnomeID )
{
	if( !g ) return;
	g->em()->startMission( type, action, targetKingdom, gnomeID );

	emit signalMissions( g->em()->missions() );
}

/// @brief Relays a mission state-change notification from the game to the GUI.
/// @param mission Updated mission record.
void AggregatorNeighbors::onUpdateMission( const Mission& mission )
{
	if( !g ) return;
	emit signalUpdateMission( mission );
}