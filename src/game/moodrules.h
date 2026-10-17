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
/** @file moodrules.h
 *  @brief Qt-free gnome mood rules: bounded memories of lived conditions, mood drift and a
 *         temporary work-refusal break. Driven once per game minute by Gnome::evalNeeds.
 *         All constants are this fork's own balance values (see docs/MOOD.md).
 */
#pragma once

#include <array>
#include <string>
#include <vector>

namespace Mood
{

/// @brief Kinds of remembered thoughts. Each kind has at most one memory slot.
enum class Thought : int
{
	Hungry = 0,
	Starving,
	Thirsty,
	Dehydrated,
	Tired,
	Exhausted,
	SleptOnFloor,
	Injured,
	Catharsis,
	Count
};
constexpr int ThoughtCount = static_cast<int>( Thought::Count );

/// @brief Effect and lifetime of one thought kind.
struct ThoughtRule
{
	int effect;        ///< Mood points while the cause applies (events: whole lifetime).
	int lingerEffect;  ///< Mood points after the cause stopped, until forgotten.
	int lingerMinutes; ///< Game minutes the memory stays after its cause last applied.
	const char* label; ///< Russian UI label (UTF-8).
};

const ThoughtRule& rule( Thought t );

// Balance constants (game minutes / mood points 0..100).
constexpr int Baseline                = 75;
constexpr int HungryBelow             = 15; ///< Hunger need below this: Hungry.
constexpr int StarvingBelow           = 0;  ///< Hunger need below this: Starving (adds to Hungry).
constexpr int ThirstyBelow            = 15;
constexpr int DehydratedBelow         = 0;
constexpr int TiredBelow              = 15;
constexpr int ExhaustedBelow          = 0;
constexpr int MoodRateTenthsPerMinute = 4;    ///< Mood moves toward its target by 0.4 per minute.
constexpr int BreakThreshold          = 30;   ///< Mood below this accumulates stress.
constexpr int StressToBreakMinutes    = 120;  ///< Stress minutes that start a break.
constexpr int StressRecoveryPerMinute = 2;    ///< Stress lost per minute at mood >= threshold.
constexpr int BreakMinutes            = 180;  ///< Duration of the work refusal.
constexpr int BreakCooldownMinutes    = 1440; ///< No stress and no new break after a break.

/// @brief Need values sampled once per game minute.
struct NeedSample
{
	double hunger = 100;
	double thirst = 100;
	double sleep  = 100;
};

/// @brief One remembered thought as shown to the player.
struct Memory
{
	Thought thought;
	int effect;     ///< Current effect: rule effect while ongoing, lingerEffect afterwards.
	int ageMinutes; ///< Minutes since the cause last applied.
	bool ongoing;   ///< Cause applied in the latest sample.
};

enum class Transition
{
	None,
	BreakStarted,
	BreakEnded
};

/// @brief Per-gnome mood state. Deterministic, bounded and serializable.
class MoodState
{
public:
	MoodState();

	/// @brief Sets the current mood (new gnome, legacy save, debug tool). Clamped to 0..100,
	///        non-finite values fall back to Baseline. Memories and timers are untouched.
	void setMood( double value );

	int mood() const;
	int target() const;

	/// @brief Advances one game minute using the current need values.
	Transition tickMinute( const NeedSample& sample );

	/// @brief Records a one-off event (sleep without bed, real injury). Repeats refresh the
	///        same slot instead of stacking.
	void noteEvent( Thought t );

	bool refusesWork() const { return m_breakRemaining > 0; }
	int breakRemaining() const { return m_breakRemaining; }
	int cooldownRemaining() const { return m_cooldownRemaining; }
	int stressMinutes() const { return m_stress; }
	bool remembers( Thought t ) const;

	/// @brief Held memories, most recent first.
	std::vector<Memory> memories() const;

	/// @brief Russian one-line status: mood level or the running break.
	std::string statusText() const;
	/// @brief Russian lines for the most recent thoughts (at most @p maxLines).
	std::vector<std::string> thoughtLines( int maxLines ) const;

	/// @brief Compact versioned text form for save games.
	std::string serialize() const;
	/// @brief Restores from serialize() output. Out-of-range numbers are clamped. Returns false
	///        and leaves the state unchanged when the text is not a valid record.
	bool deserialize( const std::string& text );

private:
	int m_moodTenths        = Baseline * 10;
	int m_stress            = 0;
	int m_breakRemaining    = 0;
	int m_cooldownRemaining = 0;
	std::array<int, ThoughtCount> m_age;      ///< -1 = not remembered.
	std::array<bool, ThoughtCount> m_ongoing; ///< Not persisted; recomputed every minute.

	void refresh( Thought t, bool ongoing );
};

/// @brief Russian word for a mood value: хорошее / ровное / плохое / очень плохое.
const char* moodLevelLabel( int mood );

} // namespace Mood
