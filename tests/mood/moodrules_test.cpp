// Tests for src/game/moodrules.cpp, the production mood rules used by Gnome::evalNeeds.
// Qt-free; build and run (see docs/MOOD.md):
//   c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -o moodrules_test tests/mood/moodrules_test.cpp src/game/moodrules.cpp
//   ./moodrules_test

#include "../../src/game/moodrules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using namespace Mood;

namespace
{
int g_failures = 0;
int g_checks   = 0;

#define CHECK( cond )                                                           \
	do                                                                          \
	{                                                                           \
		++g_checks;                                                             \
		if ( !( cond ) )                                                        \
		{                                                                       \
			++g_failures;                                                       \
			std::printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond );       \
		}                                                                       \
	} while ( 0 )

#define CHECK_EQ( a, b )                                                                                  \
	do                                                                                                    \
	{                                                                                                     \
		++g_checks;                                                                                       \
		auto va_ = ( a );                                                                                 \
		auto vb_ = ( b );                                                                                 \
		if ( !( va_ == vb_ ) )                                                                            \
		{                                                                                                 \
			++g_failures;                                                                                 \
			std::printf( "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b );                          \
		}                                                                                                 \
	} while ( 0 )

NeedSample needs( double hunger, double thirst, double sleep )
{
	NeedSample s;
	s.hunger = hunger;
	s.thirst = thirst;
	s.sleep  = sleep;
	return s;
}

const NeedSample kFull   = needs( 100, 100, 100 );
const NeedSample kSevere = needs( -10, -10, 100 ); // starving and dehydrated: target 75-60 = 15

struct RunStats
{
	int breaksStarted = 0;
	int breaksEnded   = 0;
	int firstStart    = -1; // 1-based minute of the first BreakStarted in this run
	int firstEnd      = -1;
};

RunStats run( MoodState& m, const NeedSample& s, int minutes )
{
	RunStats r;
	for ( int i = 1; i <= minutes; ++i )
	{
		auto t = m.tickMinute( s );
		if ( t == Transition::BreakStarted )
		{
			if ( r.firstStart < 0 )
				r.firstStart = i;
			++r.breaksStarted;
		}
		if ( t == Transition::BreakEnded )
		{
			if ( r.firstEnd < 0 )
				r.firstEnd = i;
			++r.breaksEnded;
		}
	}
	return r;
}

// ---------------------------------------------------------------------------------------

void testCalm()
{
	MoodState m;
	m.setMood( 90 ); // new gnomes start at 81..100
	auto r = run( m, kFull, 3 * 1440 );
	CHECK_EQ( r.breaksStarted, 0 );
	CHECK( !m.refusesWork() );
	CHECK( m.memories().empty() );
	CHECK_EQ( m.mood(), Baseline );
	CHECK_EQ( m.stressMinutes(), 0 );
	CHECK_EQ( m.statusText(), std::string( "Настроение 75 из 100: хорошее" ) );
	auto lines = m.thoughtLines( 4 );
	CHECK_EQ( lines.size(), size_t( 1 ) );
	CHECK_EQ( lines[0], std::string( "Ничего не беспокоит" ) );

	// needs that already trigger the eat/drink/sleep behaviour (below 30) but stay above
	// the memory thresholds are not remembered
	run( m, needs( 20, 16, 15 ), 600 );
	CHECK( m.memories().empty() );
	CHECK_EQ( m.mood(), Baseline );
}

void testDeprivationLowersMood()
{
	MoodState m;
	run( m, needs( 10, 100, 100 ), 1 );
	CHECK( m.remembers( Thought::Hungry ) );
	CHECK( !m.remembers( Thought::Starving ) );
	CHECK_EQ( m.target(), 65 );
	run( m, needs( 10, 100, 100 ), 30 );
	CHECK_EQ( m.mood(), 65 );

	int previous = m.mood();
	bool monotonic = true;
	for ( int i = 0; i < 60; ++i )
	{
		m.tickMinute( needs( -5, 100, 100 ) );
		monotonic = monotonic && m.mood() <= previous;
		previous  = m.mood();
	}
	CHECK( monotonic );
	CHECK( m.remembers( Thought::Starving ) );
	CHECK_EQ( m.target(), 45 );
	CHECK_EQ( m.mood(), 45 );
	auto lines = m.thoughtLines( 4 );
	CHECK_EQ( lines.size(), size_t( 2 ) );
	CHECK_EQ( lines[0], std::string( "-10 Голод (сейчас)" ) );
	CHECK_EQ( lines[1], std::string( "-20 Сильный голод (сейчас)" ) );
	CHECK_EQ( m.statusText(), std::string( "Настроение 45 из 100: плохое" ) );

	// one unmet need alone is bad but not breaking: two days without a break
	auto r = run( m, needs( -5, 100, 100 ), 2 * 1440 );
	CHECK_EQ( r.breaksStarted, 0 );
	CHECK_EQ( m.stressMinutes(), 0 );

	// sleep deprivation and other stages
	MoodState s;
	run( s, needs( 100, 100, -1 ), 1 );
	CHECK( s.remembers( Thought::Tired ) );
	CHECK( s.remembers( Thought::Exhausted ) );
	CHECK_EQ( s.target(), Baseline - 8 - 15 );

	// broken sample values are ignored, not remembered
	MoodState n;
	double nan = std::numeric_limits<double>::quiet_NaN();
	double inf = std::numeric_limits<double>::infinity();
	run( n, needs( nan, -inf, inf ), 10 );
	CHECK( n.memories().empty() );
}

void testMemoryDedupAndExpiry()
{
	MoodState m;
	CHECK_EQ( m.serialize(), std::string( "1 750 0 0 0 9 -1 -1 -1 -1 -1 -1 -1 -1 -1" ) );
	run( m, needs( 10, 100, 100 ), 10000 );
	CHECK_EQ( m.memories().size(), size_t( 1 ) );
	CHECK_EQ( m.target(), 65 );
	// ten thousand samples of the same condition leave exactly one fresh memory behind
	CHECK_EQ( m.serialize(), std::string( "1 650 0 0 0 9 0 -1 -1 -1 -1 -1 -1 -1 -1" ) );

	// lingers rule().lingerMinutes after the last minute the cause applied
	// once fed the memory weakens to its linger effect at once and stays for
	// rule().lingerMinutes after the last minute the cause applied
	const int linger = rule( Thought::Hungry ).lingerMinutes;
	m.tickMinute( kFull );
	auto mems = m.memories();
	CHECK_EQ( mems.size(), size_t( 1 ) );
	CHECK( !mems[0].ongoing );
	CHECK_EQ( mems[0].ageMinutes, 1 );
	CHECK_EQ( mems[0].effect, rule( Thought::Hungry ).lingerEffect );
	CHECK_EQ( m.target(), 70 );
	CHECK_EQ( m.thoughtLines( 4 )[0], std::string( "-5 Голод (1 мин назад)" ) );
	run( m, kFull, linger - 2 );
	CHECK( m.remembers( Thought::Hungry ) );
	CHECK_EQ( m.thoughtLines( 4 )[0], std::string( "-5 Голод (7 ч назад)" ) );
	m.tickMinute( kFull );
	CHECK( !m.remembers( Thought::Hungry ) );
	CHECK_EQ( m.target(), Baseline );

	// repeated events refresh one slot instead of stacking
	MoodState e;
	for ( int i = 0; i < 1000; ++i )
	{
		e.noteEvent( Thought::Injured );
	}
	CHECK_EQ( e.memories().size(), size_t( 1 ) );
	CHECK_EQ( e.target(), Baseline - 12 );
	CHECK_EQ( e.thoughtLines( 4 )[0], std::string( "-12 Ранение (только что)" ) );
	e.noteEvent( Thought::SleptOnFloor );
	CHECK_EQ( e.target(), Baseline - 12 - 5 );
	e.noteEvent( static_cast<Thought>( 99 ) ); // unknown kind is ignored
	e.noteEvent( Thought::Count );
	CHECK_EQ( e.memories().size(), size_t( 2 ) );

	// every kind at once is still bounded by the number of kinds and the 0..100 range
	MoodState all;
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		all.noteEvent( static_cast<Thought>( i ) );
	}
	run( all, needs( -50, -50, -50 ), 5000 );
	CHECK( static_cast<int>( all.memories().size() ) <= ThoughtCount );
	CHECK( all.target() >= 0 && all.target() <= 100 );
	CHECK( all.mood() >= 0 && all.mood() <= 100 );
	CHECK_EQ( all.thoughtLines( 4 ).size(), size_t( 4 ) );
}

void testBreakRecoveryAndCooldown()
{
	MoodState m;
	// mood falls 0.4/min from 75; it reads below 30 at minute 114, and 120 stress minutes
	// later (minute 233) the break starts
	auto r = run( m, kSevere, 232 );
	CHECK_EQ( r.breaksStarted, 0 );
	CHECK_EQ( m.statusText(), std::string( "Настроение 15 из 100: очень плохое, близок срыв" ) );
	CHECK_EQ( m.tickMinute( kSevere ), Transition::BreakStarted );
	CHECK( m.refusesWork() );
	CHECK_EQ( m.breakRemaining(), BreakMinutes );
	CHECK_EQ( m.statusText(), std::string( "Срыв: не берёт работу, ещё 3 ч" ) );

	// fixed duration even though the causes continue
	r = run( m, kSevere, BreakMinutes - 1 );
	CHECK_EQ( r.breaksEnded, 0 );
	CHECK( m.refusesWork() );
	CHECK_EQ( m.statusText(), std::string( "Срыв: не берёт работу, ещё 1 мин" ) );
	CHECK_EQ( m.tickMinute( kSevere ), Transition::BreakEnded );
	CHECK( !m.refusesWork() );
	CHECK( m.remembers( Thought::Catharsis ) );
	CHECK_EQ( m.cooldownRemaining(), BreakCooldownMinutes );

	// no new break during the cooldown, even under the same conditions
	r = run( m, kSevere, BreakCooldownMinutes );
	CHECK_EQ( r.breaksStarted, 0 );
	CHECK_EQ( m.cooldownRemaining(), 0 );

	// over ten days of unchanged misery breaks recur, but never closer than break + cooldown
	MoodState longRun;
	int last = -1, count = 0, minGap = 1 << 30;
	for ( int i = 1; i <= 10 * 1440; ++i )
	{
		if ( longRun.tickMinute( kSevere ) == Transition::BreakStarted )
		{
			if ( last >= 0 )
				minGap = std::min( minGap, i - last );
			last = i;
			++count;
		}
	}
	CHECK( count >= 2 );
	CHECK( minGap >= BreakMinutes + BreakCooldownMinutes );
	CHECK( count <= 10 * 1440 / ( BreakMinutes + BreakCooldownMinutes ) + 1 );
}

void testRecoveryAfterNeedsMet()
{
	MoodState m;
	auto r = run( m, kSevere, 233 + BreakMinutes );
	CHECK_EQ( r.firstStart, 233 );
	CHECK_EQ( r.firstEnd, 233 + BreakMinutes );

	// food and water arrive: the target improves at once (75 - 5 - 10 - 5 - 10 + 20),
	// memories linger, catharsis helps, no new break
	m.tickMinute( kFull );
	CHECK_EQ( m.target(), 65 );
	CHECK( m.remembers( Thought::Starving ) );
	int minMood = 100;
	int breaks  = 0;
	int minuteAbove50 = -1;
	for ( int i = 1; i <= 3 * 1440; ++i )
	{
		if ( m.tickMinute( kFull ) == Transition::BreakStarted )
			++breaks;
		minMood = std::min( minMood, m.mood() );
		if ( minuteAbove50 < 0 && m.mood() >= 50 )
			minuteAbove50 = i;
	}
	CHECK_EQ( breaks, 0 );
	CHECK( minMood >= 15 );
	CHECK( minuteAbove50 > 0 && minuteAbove50 <= 720 );
	CHECK( m.memories().empty() );
	CHECK_EQ( m.mood(), Baseline );
	CHECK_EQ( m.stressMinutes(), 0 );
}

void testSerialization()
{
	// round trip in the middle of a break
	MoodState a;
	run( a, kSevere, 233 + 50 );
	CHECK( a.refusesWork() );
	a.noteEvent( Thought::Injured );
	std::string record = a.serialize();
	MoodState b;
	CHECK( b.deserialize( record ) );
	CHECK_EQ( b.serialize(), record );
	CHECK_EQ( b.breakRemaining(), a.breakRemaining() );
	CHECK_EQ( b.mood(), a.mood() );
	CHECK_EQ( b.memories().size(), a.memories().size() );

	// saving and loading every minute neither resets nor extends anything
	MoodState c;
	CHECK( c.deserialize( record ) );
	bool same = true;
	for ( int i = 0; i < 2000; ++i )
	{
		NeedSample s = i < 300 ? kSevere : kFull;
		a.tickMinute( s );
		MoodState reloaded;
		same = same && reloaded.deserialize( c.serialize() );
		c    = reloaded;
		c.tickMinute( s );
		same = same && c.serialize() == a.serialize();
	}
	CHECK( same );
	CHECK( !a.refusesWork() );

	// invalid records are rejected and leave the state untouched
	MoodState d;
	d.setMood( 42 );
	const std::string before = d.serialize();
	const char* invalid[] = {
		"",
		"garbage",
		"2 750 0 0 0 9 -1 -1 -1 -1 -1 -1 -1 -1 -1", // unknown version
		"1 750 0",                                   // truncated header
		"1 750 0 0 0 9 -1 -1 -1",                    // fewer ages than announced
		"1 750 0 0 0 9 -1 -1 x -1 -1 -1 -1 -1 -1",   // not a number
		"1 750 0 0 0 9 -1 -1 -1 -1 -1 -1 -1 -1 -1 7", // more ages than announced
		"1 750 0 0 0 65",                            // implausible kind count
		"1 750 0 0 0 -1",
		"1.5 750 0 0 0 0",
	};
	for ( const char* text : invalid )
	{
		CHECK( !d.deserialize( text ) );
		CHECK_EQ( d.serialize(), before );
	}

	// extreme numbers are clamped: no overflow, no endless break
	MoodState e;
	CHECK( e.deserialize( "1 99999999999999999999999 -5 999999 -999999999999999999999 9 5 99999 -7 0 0 0 0 0 0" ) );
	CHECK_EQ( e.mood(), 100 );
	CHECK_EQ( e.stressMinutes(), 0 );
	CHECK_EQ( e.breakRemaining(), BreakMinutes );
	CHECK_EQ( e.cooldownRemaining(), 0 );
	CHECK( e.remembers( Thought::Hungry ) );     // age 5 kept
	CHECK( !e.remembers( Thought::Starving ) );  // age beyond linger dropped
	CHECK( !e.remembers( Thought::Thirsty ) );   // negative age = not remembered
	CHECK( e.remembers( Thought::Catharsis ) );
	auto r = run( e, kFull, BreakMinutes );
	CHECK_EQ( r.breaksEnded, 1 );
	CHECK( !e.refusesWork() );

	// records from other versions of the kind list
	MoodState f;
	CHECK( f.deserialize( "1 600 0 0 0 3 0 -1 0" ) );
	CHECK( f.remembers( Thought::Hungry ) && f.remembers( Thought::Thirsty ) );
	CHECK_EQ( f.memories().size(), size_t( 2 ) );
	CHECK( f.deserialize( "1 600 0 0 0 11 -1 -1 -1 -1 -1 -1 -1 -1 -1 4 4" ) );
	CHECK( f.memories().empty() );

	// legacy saves only carry Happiness; it is clamped and sanitized
	MoodState g;
	g.setMood( std::numeric_limits<double>::quiet_NaN() );
	CHECK_EQ( g.mood(), Baseline );
	g.setMood( std::numeric_limits<double>::infinity() );
	CHECK_EQ( g.mood(), Baseline );
	g.setMood( -50 );
	CHECK_EQ( g.mood(), 0 );
	g.setMood( 1e300 );
	CHECK_EQ( g.mood(), 100 );
	g.setMood( 0 ); // a legacy gnome at 0 Happiness with all needs met recovers, no break
	auto rg = run( g, kFull, 1440 );
	CHECK_EQ( rg.breaksStarted, 0 );
	CHECK_EQ( g.mood(), Baseline );
}

// A short colony story driven with the database decay rates (Hunger/Thirst -0.025 and
// Sleep -0.03 per minute). Only the sampled need values are scripted here; every mood
// decision comes from MoodState.
void testScenarioShortageAndRelief()
{
	MoodState m;
	m.setMood( 88 );
	double hunger = 95, thirst = 90, sleep = 80;
	bool supplies = true;
	int breakStart = -1, breakEnd = -1, reliefMinute = -1, breaks = 0;
	double hungerAtBreak = 0;
	std::string savedDuringBreak;

	for ( int minute = 1; minute <= 9 * 1440; ++minute )
	{
		hunger -= 0.025;
		thirst -= 0.025;
		sleep -= 0.03;
		// the gnome eats/drinks/sleeps when the behaviour tree's 30 threshold is reached,
		// but only while the colony has supplies; it always manages to sleep somewhere
		if ( supplies && hunger < 30 )
			hunger = 100;
		if ( supplies && thirst < 30 )
			thirst = 100;
		if ( sleep < 30 )
			sleep = 100;
		if ( minute == 1440 )
			supplies = false; // stores run out on day two

		auto t = m.tickMinute( needs( hunger, thirst, sleep ) );
		if ( t == Transition::BreakStarted )
		{
			++breaks;
			if ( breakStart < 0 )
			{
				breakStart    = minute;
				hungerAtBreak = hunger;
			}
		}
		if ( t == Transition::BreakEnded && breakEnd < 0 )
			breakEnd = minute;

		if ( breakStart > 0 && minute == breakStart + 60 )
		{
			// the player saves during the break and restores supplies
			savedDuringBreak = m.serialize();
			supplies         = true;
			reliefMinute     = minute;
		}
	}

	CHECK( breakStart > 1440 );
	CHECK( hungerAtBreak > -100 ); // the break comes well before starvation
	CHECK( hungerAtBreak < 0 );
	CHECK_EQ( breakEnd, breakStart + BreakMinutes ); // food does not shorten it, but it is bounded
	CHECK_EQ( breaks, 1 );                          // relief prevents any further break
	CHECK( reliefMinute > 0 );
	CHECK( m.memories().empty() );
	CHECK_EQ( m.mood(), Baseline );

	MoodState loaded;
	CHECK( loaded.deserialize( savedDuringBreak ) );
	CHECK( loaded.refusesWork() );
	CHECK_EQ( loaded.breakRemaining(), BreakMinutes - 60 );
	std::printf( "scenario: stores empty at minute 1440, break %d..%d (hunger %.1f), relief at %d\n",
		breakStart, breakEnd, hungerAtBreak, reliefMinute );
}

void testRulesTable()
{
	for ( int i = 0; i < ThoughtCount; ++i )
	{
		const auto& r = rule( static_cast<Thought>( i ) );
		CHECK( r.lingerMinutes > 0 );
		CHECK( std::string( r.label ).size() > 0 );
		CHECK( ( r.effect > 0 ) == ( static_cast<Thought>( i ) == Thought::Catharsis ) );
		CHECK( ( r.lingerEffect > 0 ) == ( r.effect > 0 ) );
		CHECK( std::abs( r.lingerEffect ) <= std::abs( r.effect ) );
	}
}
} // namespace

int main()
{
	testCalm();
	testDeprivationLowersMood();
	testMemoryDedupAndExpiry();
	testBreakRecoveryAndCooldown();
	testRecoveryAfterNeedsMet();
	testSerialization();
	testScenarioShortageAndRelief();
	testRulesTable();

	std::printf( "%d checks, %d failures\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}
