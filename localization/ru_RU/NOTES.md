# Russian localization: maintainer notes

## Files

| File | Content |
|---|---|
| `translation.tsv` | All 1,227 rows of the `Translation` table: `id`, upstream English text (reference), Russian text. |
| `db_extra.tsv` | Other displayed database cells: event popups (`Events_*`) and `Words_Numbers`. |
| `GLOSSARY.md` | Term choices. |
| `content/xaml/localization/ru_RU.xaml` | All 251 keys of `en_US.xaml`; every active page merges this file. |
| `content/xaml/**.xaml` | Literal labels translated in place; IDs that double as commands are mapped to Russian with `DataTrigger`s (tile-info tabs, terrain actions, command/build buttons, schedule letters, profession `Gnomad`). |
| `content/JSON/*.json` | Default window, `language: ru_RU`, profession and embark preset names. |

`content/db/ingnomia.db.sql` is generated in place from the TSV files:

    python3 tools/l10n/ru_locale.py apply-sql --repo .
    python3 tools/l10n/ru_locale.py apply-sql --repo . --direction en   # back to upstream English

The patch only replaces the text cells listed in the TSV files and refuses to run when
a cell matches neither the English nor the Russian value. Russian values never contain
`;`, because `DB::init` splits the dump on every semicolon.

## Checks

    git show 4f99266c0f95faa847ca0db2af18cf59aff07f4b:content/db/ingnomia.db.sql > /tmp/upstream.db.sql
    python3 tools/l10n/ru_locale.py check --repo . --reference-sql /tmp/upstream.db.sql
    python3 tools/l10n/ru_locale.py scan-cpp --repo .
    python3 tools/l10n/ru_locale.py check-package --dir <unpacked package> --repo .

`check` fails on: missing or empty Russian values, values without Cyrillic, Latin words
outside the proper-name list, changed placeholders (`$Num`, `%1`, `{0}`), `;` in values,
SQL that is not fully localized, statements that fail the game's naive import, any SQL
difference from upstream other than the localized cells, XAML parse errors, pages still
merging `en_US.xaml`, English display text in active XAML, non-Russian default config,
and fonts that are missing or lack a glyph used by the Russian texts.
`scan-cpp` is a heuristic report of English literals in display-related C++.

## Decisions and exceptions

- Gnome first names (`Names` table) stay in Latin script: they are proper names and the
  column is used as the stored name.
- Kingdom names: the English `Words_*` tables cannot be recombined grammatically in
  Russian, so `Strings::randomKingdomName()` uses a curated Russian set in `strings.cpp`.
  The upstream generator code is kept but no longer called; the tables are untouched.
- `numberWord()` returns digits; Russian number words would need case agreement.
- Creature logs (`Creature::log`, `m_log`), the combat logger, world-generator status and
  `qDebug` output are not shown by the 0.9.0 GUI and stay English.
- `menusettings.json` is not read by the 0.9.0 code and is left as is.
- Source quirks kept by meaning: `$ItemName_Radish` says "fruit" → «плод»;
  `$MaterialName_Sugarbeet` says "sugar" → «сахарная свёкла» (the material is the plant).
- Fonts: the main menu and in-game GUI use `Fonts/#WeblySleek UI Semibold`
  (`weblysleekuisb.ttf`, Cyrillic). The upstream theme default `PT Root UI` (tooltips,
  dropdowns) lives in `content/xaml/Theme/Fonts/`, which ships with the official release;
  the packager fills it from the pinned release ZIP when a build tree lacks it.

## Data-only package limitations (unmodified 0.9.0 executable)

The C++ changes of the fork are absent, so: date/time, tile info, neighbors, trade and
kingdom counters stay English; kingdom names are English; item names read
«железо кирка»; the season clock icon always shows winter because the original code
compares the translated season name with English words; the language list shows
`en_US`/`fr_FR`. The Russian README of the package states this.
