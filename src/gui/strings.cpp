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
/** @file strings.cpp
 *  @brief Strings implementation: loads the Translation DB table into the in-memory lookup
 *         table, provides lookup with an obvious "Ошибка: <key>" fallback, Russian display
 *         helpers (material/item names, plural forms) and random Russian kingdom names.
 */
#include "strings.h"

#include "../base/config.h"
#include "../base/db.h"

#include <QApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QVariantMap>

#include <chrono>
#include <cstddef>
#include <cstdlib>

QMap<QString, QString> Strings::m_table = QMap<QString, QString>();
QString Strings::m_language             = "";

/// @brief Private constructor (singleton pattern).
Strings::Strings()
{
}

/// @brief Destructor.
Strings::~Strings()
{
}

/// @brief Loads the Translation DB table into the in-memory key → string map. The current
///        language is read from Global::cfg["language"].
/// @return Always true (kept as bool for future error reporting).
bool Strings::init()
{
	m_language = Global::cfg->get( "language" ).toString();
	m_table.clear();
	//for( auto row : DB::selectRows( "Translation_" + m_language ) )
	for ( auto row : DB::selectRows( "Translation" ) )
	{
		m_table.insert( row.value( "ID" ).toString(), row.value( "Text" ).toString() );
	}

	return true;
}

/// @brief Looks up a localised string by key. Returns "Error: <key>" when the key is
///        missing so untranslated strings are visible in the GUI during development.
/// @param key Translation key (e.g. "$ItemName_IronSword").
/// @return Localised string, or an error placeholder.
QString Strings::s( QString key )
{
	if ( m_table.contains( key ) )
	{
		return m_table[key];
	}
	else
	{
		return "Ошибка: " + key;
		//return m_table["$ERROR_TranslationKeyUnknown"].toString();
	}
}

/// @brief Inserts or overwrites a single translation at runtime (used for dynamic strings
///        like gnome names that aren't present in the DB).
/// @param key    Translation key.
/// @param string Localised value.
void Strings::insertString( QString key, QString string )
{
	m_table.insert( key, string );
}

QString Strings::sOr( const QString& key, const QString& fallback )
{
	return m_table.value( key, fallback );
}

QString Strings::materialItem( const QString& materialSID, const QString& itemSID )
{
	return s( "$ItemName_" + itemSID ) + " (" + s( "$MaterialName_" + materialSID ) + ")";
}

QString Strings::plural( int n, const QString& one, const QString& few, const QString& many )
{
	const int n100 = qAbs( n ) % 100;
	const int n10  = n100 % 10;
	if ( n100 >= 11 && n100 <= 14 )
	{
		return many;
	}
	if ( n10 == 1 )
	{
		return one;
	}
	if ( n10 >= 2 && n10 <= 4 )
	{
		return few;
	}
	return many;
}

namespace
{
// Curated Russian name parts for kingdom names. The English Words_* tables can't be
// recombined grammatically in Russian, so the Russian-first fork uses its own small set.
struct RuNoun
{
	const char* nominative;
	const char* genitive;
	int gender; // 0 masculine, 1 feminine, 2 neuter
};

struct RuAdjective
{
	const char* forms[3]; // masculine, feminine, neuter
};

const RuNoun ruNouns[] = {
	{ "Молот", "Молота", 0 },
	{ "Котёл", "Котла", 0 },
	{ "Утёс", "Утёса", 0 },
	{ "Холм", "Холма", 0 },
	{ "Перевал", "Перевала", 0 },
	{ "Бор", "Бора", 0 },
	{ "Ручей", "Ручья", 0 },
	{ "Кряж", "Кряжа", 0 },
	{ "Горн", "Горна", 0 },
	{ "Гриб", "Гриба", 0 },
	{ "Долина", "Долины", 1 },
	{ "Наковальня", "Наковальни", 1 },
	{ "Пещера", "Пещеры", 1 },
	{ "Кирка", "Кирки", 1 },
	{ "Чаща", "Чащи", 1 },
	{ "Гора", "Горы", 1 },
	{ "Жила", "Жилы", 1 },
	{ "Шахта", "Шахты", 1 },
	{ "Ущелье", "Ущелья", 2 },
	{ "Озеро", "Озера", 2 },
	{ "Подземелье", "Подземелья", 2 },
	{ "Огниво", "Огнива", 2 },
};

const RuAdjective ruAdjectives[] = {
	{ { "Каменный", "Каменная", "Каменное" } },
	{ { "Медный", "Медная", "Медное" } },
	{ { "Железный", "Железная", "Железное" } },
	{ { "Золотой", "Золотая", "Золотое" } },
	{ { "Серебряный", "Серебряная", "Серебряное" } },
	{ { "Дубовый", "Дубовая", "Дубовое" } },
	{ { "Грибной", "Грибная", "Грибное" } },
	{ { "Тихий", "Тихая", "Тихое" } },
	{ { "Северный", "Северная", "Северное" } },
	{ { "Глубокий", "Глубокая", "Глубокое" } },
	{ { "Звонкий", "Звонкая", "Звонкое" } },
	{ { "Ржавый", "Ржавая", "Ржавое" } },
	{ { "Мшистый", "Мшистая", "Мшистое" } },
	{ { "Туманный", "Туманная", "Туманное" } },
	{ { "Гранитный", "Гранитная", "Гранитное" } },
	{ { "Хмельной", "Хмельная", "Хмельное" } },
	{ { "Вольный", "Вольная", "Вольное" } },
	{ { "Полночный", "Полночная", "Полночное" } },
	{ { "Янтарный", "Янтарная", "Янтарное" } },
	{ { "Ясный", "Ясная", "Ясное" } },
};

const char* ruPrefixes[] = { "Камне", "Медно", "Железно", "Грибо", "Мохо", "Злато", "Сребро", "Дубо", "Кремне", "Рудо" };
const char* ruSuffixes[] = { "град", "дол", "бор", "горье" };

template <typename T, std::size_t N>
const T& pick( const T ( &list )[N] )
{
	return list[rand() % N];
}
} // namespace

/// @brief Generates a random Russian kingdom name from the curated parts above.
///        The upstream English generator (Namerules_Rule "Faction" + Words_* tables) is kept
///        in replaceNamePart() for reference but no longer called.
/// @return Freshly generated kingdom name.
QString Strings::randomKingdomName()
{
	srand( std::chrono::system_clock::now().time_since_epoch().count() );

	const RuNoun& noun     = pick( ruNouns );
	const RuAdjective& adj = pick( ruAdjectives );

	switch ( rand() % 5 )
	{
		case 0:
			return QString( adj.forms[noun.gender] ) + " " + noun.nominative;
		case 1:
			return QString( "Королевство " ) + noun.genitive;
		case 2:
			return QString( adj.forms[2] ) + " королевство";
		case 3:
			return QString( "Земли " ) + noun.genitive;
		default:
			return QString( pick( ruPrefixes ) ) + pick( ruSuffixes );
	}
}

/// @brief Resolves one name-rule part into an actual word. Reserved words ("The", "of",
///        "Land", "Kingdom") are returned verbatim; otherwise the part is split on '_' and
///        dispatched to replaceNamePart2() based on arity.
/// @param part Name-rule token.
/// @return Resolved word with the first letter upper-cased.
QString Strings::replaceNamePart( QString part )
{
	QSet<QString> noReplaceables = { "The", "of", "Land", "Kingdom" };
	if ( noReplaceables.contains( part ) )
	{
		return part;
	}
	QStringList pl = part.split( "_" );
	QString out;

	switch ( pl.size() )
	{
		case 1: // only "Adjective" so far
			out = replaceNamePart2( pl.at( 0 ) );
			break;
		case 2:
			out = replaceNamePart2( pl.at( 0 ), pl.at( 1 ) );
			break;
		case 3: // comppounds
		{
			if ( pl.at( 0 ) == "Compound" )
			{
				out = replaceNamePart2( pl.at( 1 ), pl.at( 2 ) );
				out += replaceNamePart2( pl.at( 1 ), pl.at( 2 ) );
			}
		}
		break;
	}
	out.replace( 0, 1, out[0].toUpper() );
	return out;
}

/// @brief Picks a random row from the Words_<part> table and returns its "Word" column.
/// @param part Word category (e.g. "Adjective").
/// @return Randomly chosen word.
QString Strings::replaceNamePart2( QString part )
{
	QString tableName = "Words_" + part;
	int ra            = rand() % DB::numRows( tableName );
	return DB::select( "Word", tableName, ra ).toString();
}

/// @brief Picks a random row from the Words_<part> table and returns a specified column.
///        "Singular" is normalised to "Word" to match the column naming in most tables.
/// @param part  Word category.
/// @param part2 Column name to read (or "Singular").
/// @return Randomly chosen word in the requested form.
QString Strings::replaceNamePart2( QString part, QString part2 )
{
	if ( part2 == "Singular" )
	{
		part2 = "Word";
	}
	QString tableName = "Words_" + part;
	int ra            = rand() % DB::numRows( tableName );
	return DB::select( part2, tableName, ra ).toString();
}

/// @brief Returns a display form for a non-negative integer. Russian number words would
///        need case agreement with the following noun, so digits are used instead.
///        Negative values return "нет числа".
/// @param number Integer to display.
/// @return Numeric string, or "нет числа".
QString Strings::numberWord( int number )
{
	if ( number >= 0 )
	{
		return QString::number( number );
	}
	return "нет числа";
}