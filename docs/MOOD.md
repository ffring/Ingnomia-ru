# Gnome mood, memories and work-refusal breaks

This fork adds one small mechanic to Ingnomia 0.9.0: what a colony gnome lives through
leaves bounded memories, the memories set its mood (the existing Happiness bar), and a
mood that stays very low for long enough makes the gnome refuse new work for a while.
Everything else (needs decay, eating, sleeping, combat, events) is unchanged.

Code: `src/game/moodrules.{h,cpp}` (Qt-free rules), wired in `src/game/gnome.cpp`
(`evalNeeds`/`evalMood`, save/load, injury hook), `src/game/gnomeactions.cpp`
(`actionGetJob`, `actionFindBed`, `actionSleep`), `src/game/anatomy.cpp` (`damage` now
reports whether a hit got through armor) and the creature panel
(`src/gui/aggregatorcreatureinfo.cpp`, `src/gui/xaml/creatureinfomodel.*`,
`content/xaml/CreatureInfo.xaml`).

## Rules

All times are game minutes (1 game day = 1440). The rules run once per game minute for
gnomes of type `GNOME` that are alive and not on a mission (the same place where needs
decay). Automatons, traders, animals and monsters have no mood.

### Memories

One memory slot per kind, so at most 9 memories exist and repeated sampling or repeated
events only refresh a slot, never stack it. A condition refreshes its memory every minute
it holds and has its full effect while it holds; once the cause is gone the memory keeps a
weaker effect and is forgotten `linger` minutes after the last refresh. Events keep one
effect for their whole lifetime.

| Kind | Cause | While it holds | Afterwards | Linger | Label |
|---|---|---|---|---|---|
| Hungry | Hunger need < 15 | -10 | -5 | 480 | Голод |
| Starving | Hunger need < 0 | -20 | -10 | 720 | Сильный голод |
| Thirsty | Thirst need < 15 | -10 | -5 | 480 | Жажда |
| Dehydrated | Thirst need < 0 | -20 | -10 | 720 | Сильная жажда |
| Tired | Sleep need < 15 | -8 | -4 | 360 | Усталость |
| Exhausted | Sleep need < 0 | -15 | -8 | 600 | Изнеможение |
| SleptOnFloor | event: woke up rested, not on the bed found by FindBed | -5 | -5 | 1440 | Сон на полу |
| Injured | event: a hit that did damage after armor (not dodged, not fully absorbed) | -12 | -12 | 720 | Ранение |
| Catharsis | event: end of a break | +20 | +20 | 1440 | Разрядка после срыва |

Stages add up: a starving gnome holds both Hungry and Starving (-30). The need
thresholds sit below the behaviour tree's own eat/drink/sleep trigger (30), so a gnome
that can reach food, drink and a sleeping place forms no deprivation memories at all.

### Mood

- target = clamp(75 + sum of held effects, 0, 100)
- mood moves toward the target by 0.4 per minute (stored in tenths)
- mood is written to the `Happiness` need every minute, so the existing purple bar shows it

### Break

- mood < 30: +1 stress per minute; otherwise -2 per minute (never below 0)
- 120 stress minutes start a break: for 180 minutes `actionGetJob` returns FAILURE
- the break ends after exactly 180 minutes, adds Catharsis (+20) and starts a 1440-minute
  cooldown during which stress stays 0 and no break can start

So breaks are at least 1620 minutes apart. Example with constant starvation and
dehydration (target 15) from mood 75: mood reads below 30 at minute 114, the break starts
at minute 233 and ends at 413. If food and drink arrive then, the target rises at once to
65 (75 - 30 lingering + 20 catharsis), mood passes 50 about 1.5 hours later and is back at
75 with no memories a day later (all checked in the tests). One unmet need alone (target
45) makes the mood bad but does not cause a break; a second severe cause or an injury does.

What a break does in the behaviour tree (`content/ai/gnome_standard.xml`, unchanged):
only the `GetJob` action refuses. Alarm, combat, missions and critical needs are earlier
branches of `MainTree` and run as before; uniform checks and military training also come
before `Jobs`. A job the gnome already holds is not aborted: it is finished or aborted by
the normal job subtree, so claimed items and work positions are released by the existing
code paths. Without a job the gnome falls through to `NonCriticalNeeds` and `RandomMove`.

### Player view

The creature window shows, under the Happiness bar, a status line and up to four most
recent memories, for example:

    Настроение 45 из 100: плохое
    -10 Голод (сейчас)
    -20 Сильный голод (сейчас)

During a break: `Срыв: не берёт работу, ещё 2 ч 10 мин`. Break start and end are also
written to the gnome's own log. Monsters and animals get an empty mood text and the panel
no longer keeps the previous gnome's profession or equipment for them.

### Saves

`Gnome::serialize` stores `"Mood"` as a versioned text record:
`1 <mood tenths> <stress> <break left> <cooldown left> <kind count> <age per kind...>`
(age -1 = not held). Loading clamps every number to its valid range (mood 0..1000 tenths,
stress 0..120, break 0..180, cooldown 0..1440, ages beyond linger are dropped), so a
corrupted save cannot produce a permanent break or an overflow. A record that does not
parse is ignored. Saves without a valid record (all saves made before this change) start
from their stored `Happiness`, clamped to 0..100, with no memories and no timers. New
gnomes start from the Happiness value the gnome factory already assigns (81..100), then
drift to 75. Setting Happiness through the debug need tool sets the mood. The gnome also
saves `"SleepBedPos"`, the bed from its last FindBed (see Limits).

## Sources and what was adapted

No formula was copied; every number above is this fork's own balance choice.

- RimWorld (https://rimworldgame.com/ and Tynan Sylvester, GDC 2017, "RimWorld: Contrarian,
  Ridiculous, and Impossible Game Design Methods", pages 4, 9, 10): observable needs and
  wounds feeding a visible mood, mood-driven breakdowns, loss followed by recovery, and
  visible feedback. Adapted here as: memories with visible causes, a mood target with
  gradual drift, a break threshold with a delay, a timed break and a catharsis bonus.
- Dwarf Fortress dev log (https://www.bay12games.com/dwarves/dev_2018.html, April 14 and
  May 5 entries; https://www.bay12games.com/dwarves/dev_2015.html on needs): bounded
  memories of event plus emotion that fade, effects that persist after the event, and only
  actual exposure counting. Adapted here as: one slot per kind with a linger time, and
  injuries counted only when damage gets through.

Not implemented: personality traits, relationships, memory promotion, social thoughts,
storyteller changes, new events or AI.

## Limits

- Mood stops while a gnome is on a mission, together with its needs.
- A break blocks every kind of new job, including food production. Bounded to 180 minutes
  and at most one per 1620 minutes.
- `SleptOnFloor` is judged by position when sleep starts: sleeping on the bed found by the
  last FindBed counts as a bed. The thought bubble is not saved, so a sleep resumed after
  loading is judged again; the FindBed result is saved as `"SleepBedPos"` (format of
  `"CurrentTargetPos"`), so walking to a bed or sleeping in it survives save/load. Saves
  without that field give no floor penalty for the one sleep resumed from the save; the
  next FindBed makes the bed known again, so later floor sleep is penalised as usual. An
  unknown bed stays unknown through any intervening re-save/reload.
- The thresholds for Tired/Exhausted are rarely reached because gnomes sleep on the floor
  when no bed exists; the usual cause is a starving gnome that keeps waking up to eat.
- The quick "release" ZIP (original 0.9.0 exe plus Russian data) has no mood mechanic; its
  creature window shows empty lines where the mood text would be.

## Verification

Automated (production `moodrules.cpp`, no Qt):

    c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -o /tmp/moodrules_test tests/mood/moodrules_test.cpp src/game/moodrules.cpp
    /tmp/moodrules_test

The CI job `mood-rules` in `.github/workflows/cmake.yml` runs the same on every push. The
game wiring (behaviour tree, save/load through `QVariantMap`, UI) is compiled by the
Windows build but has no automated gameplay test. The bed decision across save/load was
reviewed in source only: `Gnome::serialize` and the loading constructor (`src/game/gnome.cpp`),
`actionFindBed` and `actionSleep` (`src/game/gnomeactions.cpp`), the `Sleepy`/`CriticalSleep`
trees (FindBed always runs before Sleep), and `Gnome::init`, which restores the saved
tree state. Manual check with the game's debug
window (`DebugGui.xaml`, sliders «Голод:»/«Жажда:» with range -100..100 and «Задать»): select
a gnome, set Hunger and Thirst to -10 and open its creature window. With food and drink in
reach the gnome goes to eat and drink, the memories switch from «сейчас» to «… назад» and
the mood settles around 45. Without reachable food and drink the mood falls below 30 and,
about four game hours after the needs were set, the break status appears while the gnome
stops taking jobs but keeps looking for food; set both back to 100 and the mood recovers.
This manual check has not been performed for this change.
