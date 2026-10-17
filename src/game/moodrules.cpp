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
/** @file moodrules.cpp
 *  @brief Gnome mood rules. Kept free of Qt so tests/mood can build it with a plain compiler.
 */
#include "moodrules.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace Mood
{

namespace
{
// effect while the cause applies, effect afterwards, linger minutes, label.
// Conditions weaken to half once fixed; events keep their effect until forgotten.
const ThoughtRule kRules[ThoughtCount] = {
	{ -10, -5, 480, "Голод" },
	{ -20, -10, 720, "Сильный голод" },
	{ -10, -5, 480, "Жажда" },
	{ -20, -10, 720, "Сильная жажда" },
	{ -8, -4, 360, "Усталость" },
	{ -15, -8, 600, "Изнеможение" },
	{ -5, -5, 1440, "Сон на полу" },
	{ -12, -12, 720, "Ранение" },
	{ 20, 20, 1440, "Разрядка после срыва" },
};

const ThoughtRule kNoRule = { 0, 0, 1, "" };

constexpr int kFormatVersion = 1;
constexpr long long kMaxStoredKinds = 64;

int kindIndex( Thought t )
{
	return static_cast<int>( t );
}

bool validKind( Thought t )
{
	return kindIndex( t ) >= 0 && kindIndex( t ) < ThoughtCount;
}

bool below( double value, int limit )
{
	return std::isfinite( value ) && value < limit;
}

int currentEffect( int i, bool ongoing )
{
	return ongoing ? kRules[i].effect : kRules[i].lingerEffect;
}

long long clampLL( long long v, long long lo, long long hi )
{
	return std::min( hi, std::max( lo, v ) );
}

std::string duration( int minutes )
{
	int h = minutes / 60;
	int m = minutes % 60;
	if ( h > 0 && m > 0 )
	{
		return std::to_string( h ) + " ч " + std::to_string( m ) + " мин";
	}
	if ( h > 0 )
	{
		return std::to_string( h ) + " ч";
	}
	return std::to_string( m ) + " мин";
}

bool parseInt( const std::string& token, long long& out )
{
	if ( token.empty() )
	{
		return false;
	}
	errno       = 0;
	char* end   = nullptr;
	long long v = std::strtoll( token.c_str(), &end, 10 );
	if ( end != token.c_str() + token.size() )
	{
		return false;
	}
	// ERANGE saturates to LLONG_MIN/MAX; the caller clamps it like any other extreme value
	out = v;
	return true;
}
} // namespace

const ThoughtRule& rule( Thought t )
{
	if ( !validKind( t ) )
	{
		return kNoRule;
	}
	return kRules[kindIndex( t )];
}

const char* moodLevelLabel( int mood )
{
	if ( mood >= 70 )
		return "хорошее";
	if ( mood >= 50 )
		return "ровное";
	if ( mood >= BreakThreshold )
		return "плохое";
	return "очень плохое";
}

MoodState::MoodState()
{
	m_age.fill( -1 );
	m_ongoing.fill( false );
}

void MoodState::setMood( double value )
{
	if ( !std::isfinite( value ) )
	{
		value = Baseline;
	}
	value        = std::min( 100.0, std::max( 0.0, value ) );
	m_moodTenths = static_cast<int>( std::lround( value * 10 ) );
}

int MoodState::mood() const
{
	return ( m_moodTenths + 5 ) / 10;
}

int MoodState::target() const
{
	int sum = Baseline;
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		if ( m_age[i] >= 0 )
		{
			sum += currentEffect( i, m_ongoing[i] );
		}
	}
	return std::min( 100, std::max( 0, sum ) );
}

bool MoodState::remembers( Thought t ) const
{
	return validKind( t ) && m_age[kindIndex( t )] >= 0;
}

void MoodState::refresh( Thought t, bool ongoing )
{
	if ( !validKind( t ) )
	{
		return;
	}
	m_age[kindIndex( t )] = 0;
	if ( ongoing )
	{
		m_ongoing[kindIndex( t )] = true;
	}
}

void MoodState::noteEvent( Thought t )
{
	refresh( t, false );
}

Transition MoodState::tickMinute( const NeedSample& sample )
{
	// 1. memories age; a memory is dropped lingerMinutes after its cause last applied
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		m_ongoing[i] = false;
		if ( m_age[i] >= 0 && ++m_age[i] >= kRules[i].lingerMinutes )
		{
			m_age[i] = -1;
		}
	}

	// 2. conditions lived through this minute refresh their single slot
	if ( below( sample.hunger, HungryBelow ) )
		refresh( Thought::Hungry, true );
	if ( below( sample.hunger, StarvingBelow ) )
		refresh( Thought::Starving, true );
	if ( below( sample.thirst, ThirstyBelow ) )
		refresh( Thought::Thirsty, true );
	if ( below( sample.thirst, DehydratedBelow ) )
		refresh( Thought::Dehydrated, true );
	if ( below( sample.sleep, TiredBelow ) )
		refresh( Thought::Tired, true );
	if ( below( sample.sleep, ExhaustedBelow ) )
		refresh( Thought::Exhausted, true );

	// 3. mood drifts toward the memory-based target
	int goal = target() * 10;
	if ( m_moodTenths < goal )
	{
		m_moodTenths = std::min( goal, m_moodTenths + MoodRateTenthsPerMinute );
	}
	else
	{
		m_moodTenths = std::max( goal, m_moodTenths - MoodRateTenthsPerMinute );
	}

	// 4. stress, break and cooldown
	Transition result = Transition::None;
	if ( m_breakRemaining > 0 )
	{
		m_stress = 0;
		if ( --m_breakRemaining == 0 )
		{
			m_cooldownRemaining = BreakCooldownMinutes;
			refresh( Thought::Catharsis, false );
			result = Transition::BreakEnded;
		}
	}
	else if ( m_cooldownRemaining > 0 )
	{
		--m_cooldownRemaining;
		m_stress = 0;
	}
	else
	{
		if ( mood() < BreakThreshold )
		{
			m_stress = std::min( StressToBreakMinutes, m_stress + 1 );
		}
		else
		{
			m_stress = std::max( 0, m_stress - StressRecoveryPerMinute );
		}
		if ( m_stress >= StressToBreakMinutes )
		{
			m_breakRemaining = BreakMinutes;
			m_stress         = 0;
			result           = Transition::BreakStarted;
		}
	}
	return result;
}

std::vector<Memory> MoodState::memories() const
{
	std::vector<Memory> out;
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		if ( m_age[i] >= 0 )
		{
			out.push_back( { static_cast<Thought>( i ), currentEffect( i, m_ongoing[i] ), m_age[i], m_ongoing[i] } );
		}
	}
	std::stable_sort( out.begin(), out.end(), []( const Memory& a, const Memory& b ) { return a.ageMinutes < b.ageMinutes; } );
	return out;
}

std::string MoodState::statusText() const
{
	if ( refusesWork() )
	{
		return "Срыв: не берёт работу, ещё " + duration( m_breakRemaining );
	}
	std::string text = "Настроение " + std::to_string( mood() ) + " из 100: " + moodLevelLabel( mood() );
	if ( mood() < BreakThreshold && m_cooldownRemaining == 0 )
	{
		text += ", близок срыв";
	}
	return text;
}

std::vector<std::string> MoodState::thoughtLines( int maxLines ) const
{
	std::vector<std::string> lines;
	if ( maxLines <= 0 )
	{
		return lines;
	}
	auto mems = memories();
	if ( mems.empty() )
	{
		lines.push_back( "Ничего не беспокоит" );
		return lines;
	}
	for ( const auto& m : mems )
	{
		if ( static_cast<int>( lines.size() ) >= maxLines )
		{
			break;
		}
		std::string when;
		if ( m.ongoing )
			when = "сейчас";
		else if ( m.ageMinutes == 0 )
			when = "только что";
		else if ( m.ageMinutes < 60 )
			when = std::to_string( m.ageMinutes ) + " мин назад";
		else
			when = std::to_string( m.ageMinutes / 60 ) + " ч назад";
		std::string sign = m.effect > 0 ? "+" : "";
		lines.push_back( sign + std::to_string( m.effect ) + " " + rule( m.thought ).label + " (" + when + ")" );
	}
	return lines;
}

std::string MoodState::serialize() const
{
	std::ostringstream out;
	out << kFormatVersion << ' ' << m_moodTenths << ' ' << m_stress << ' ' << m_breakRemaining << ' ' << m_cooldownRemaining << ' ' << ThoughtCount;
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		out << ' ' << m_age[i];
	}
	return out.str();
}

bool MoodState::deserialize( const std::string& text )
{
	std::istringstream in( text );
	std::vector<long long> values;
	std::string token;
	while ( in >> token )
	{
		long long v = 0;
		if ( !parseInt( token, v ) || values.size() > static_cast<size_t>( 6 + kMaxStoredKinds ) )
		{
			return false;
		}
		values.push_back( v );
	}
	if ( values.size() < 6 || values[0] != kFormatVersion )
	{
		return false;
	}
	long long kinds = values[5];
	if ( kinds < 0 || kinds > kMaxStoredKinds || static_cast<long long>( values.size() ) != 6 + kinds )
	{
		return false;
	}

	MoodState s;
	s.m_moodTenths        = static_cast<int>( clampLL( values[1], 0, 1000 ) );
	s.m_stress            = static_cast<int>( clampLL( values[2], 0, StressToBreakMinutes ) );
	s.m_breakRemaining    = static_cast<int>( clampLL( values[3], 0, BreakMinutes ) );
	s.m_cooldownRemaining = static_cast<int>( clampLL( values[4], 0, BreakCooldownMinutes ) );
	// kinds unknown to this version are ignored, kinds missing from the record stay forgotten
	for ( int i = 0; i < ThoughtCount && i < kinds; ++i )
	{
		long long age = values[6 + i];
		s.m_age[i]    = ( age < 0 || age >= kRules[i].lingerMinutes ) ? -1 : static_cast<int>( age );
	}
	*this = s;
	return true;
}

} // namespace Mood
