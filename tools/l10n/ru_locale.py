#!/usr/bin/env python3
"""Russian locale tooling for the Ingnomia fork (Python 3.8+, standard library only).

Subcommands
  apply-sql      Patch Translation rows and the db_extra.tsv cells of ingnomia.db.sql.
                 Idempotent: rows that already hold the Russian text are left alone.
  check          Validate the repository: TSV/XAML coverage, placeholders, XML, SQL import
                 with the game's naive ';' split, unchanged gameplay rows (with a reference
                 SQL), English leftovers in active XAML, fonts with Cyrillic coverage.
  check-package  Validate an unpacked Windows package directory.
  scan-cpp       Heuristic list of English string literals left in display-related C++.

Every check prints its findings and the command exits non-zero when an error is found.
"""

import argparse
import collections
import json
import re
import sqlite3
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

TRANSLATION_ROWS = 1227
XAML_KEYS = 251

LOCALE_DIR = Path( "localization/ru_RU" )
TSV_PATH = LOCALE_DIR / "translation.tsv"
EXTRA_PATH = LOCALE_DIR / "db_extra.tsv"
SQL_PATH = Path( "content/db/ingnomia.db.sql" )
XAML_DIR = Path( "content/xaml" )
EN_XAML = XAML_DIR / "localization/en_US.xaml"
RU_XAML = XAML_DIR / "localization/ru_RU.xaml"
CONFIG_JSON = Path( "content/JSON/config.json" )

# XAML string keys whose Russian value may stay non-Cyrillic: section markers that are
# never displayed, and the product name.
XAML_LATIN_OK = { "MainPage_Ingnomia", "InGamePage_Ingnomia" }

# Latin words that may legitimately appear in Russian UI text.
PROPER_NAMES = { "Ingnomia", "Gnomad", "Gnomoria", "OpenGL", "Noesis", "NoesisGUI", "Steam", "Qt", "Windows" }

# Inactive data: menusettings.json is not read by the 0.9.0 code (and is not strict JSON).
INACTIVE_JSON = { "menusettings.json" }

PLACEHOLDER_RE = re.compile( r"%\d+|%[sdf]|\{\d+\}|\$[A-Za-z]+|\\[nt]" )
CYRILLIC_RE = re.compile( "[Ѐ-ӿ]" )
LATIN_WORD_RE = re.compile( r"[A-Za-z]{2,}" )
HEX_COLOR_RE = re.compile( r"^#(?:[0-9A-Fa-f]{3,4}|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$" )

TRANSLATION_LINE_RE = re.compile(
	r"^INSERT INTO \"Translation\" \(\"ID\",\"Text\"\) VALUES \('((?:[^']|'')*)','((?:[^']|'')*)'\);\s*$" )
INSERT_RE = re.compile( r"^INSERT INTO \"([^\"]+)\" \(([^)]*)\) VALUES \((.*)\);\s*$" )


class Report:
	def __init__( self ):
		self.errors = []
		self.warnings = []
		self.notes = []

	def error( self, msg ):
		self.errors.append( msg )

	def warn( self, msg ):
		self.warnings.append( msg )

	def note( self, msg ):
		self.notes.append( msg )

	def emit( self, title ):
		print( "== " + title )
		for n in self.notes:
			print( "  ok    " + n )
		for w in self.warnings:
			print( "  WARN  " + w )
		for e in self.errors:
			print( "  ERROR " + e )
		print( "== %d error(s), %d warning(s)" % ( len( self.errors ), len( self.warnings ) ) )
		return 1 if self.errors else 0


# ---------------------------------------------------------------------------------------
# TSV loading

def sql_unescape( s ):
	return s.replace( "''", "'" )


def sql_escape( s ):
	return s.replace( "'", "''" )


def read_tsv( path, columns ):
	rows = []
	with open( path, encoding="utf-8", newline="" ) as f:
		lines = f.read().splitlines()
	if not lines or lines[0].split( "\t" ) != columns:
		raise ValueError( "%s: header must be %s" % ( path, "\\t".join( columns ) ) )
	for n, line in enumerate( lines[1:], start=2 ):
		if not line.strip():
			continue
		parts = line.split( "\t" )
		if len( parts ) != len( columns ):
			raise ValueError( "%s:%d: expected %d tab-separated fields, got %d" % ( path, n, len( columns ), len( parts ) ) )
		row = dict( zip( columns, parts ) )
		row["_line"] = n
		rows.append( row )
	return rows


def load_translation( root ):
	return read_tsv( root / TSV_PATH, [ "id", "en", "ru" ] )


def load_extra( root ):
	return read_tsv( root / EXTRA_PATH, [ "table", "key_column", "key", "column", "en", "ru" ] )


def check_value_text( rep, where, en, ru ):
	if not ru.strip():
		rep.error( "%s: empty Russian value" % where )
		return
	if ";" in ru:
		rep.error( "%s: ';' in Russian value breaks the game's naive SQL split" % where )
	if "\t" in ru or "\n" in ru:
		rep.error( "%s: tab/newline in Russian value" % where )
	if not CYRILLIC_RE.search( ru ):
		rep.error( "%s: Russian value has no Cyrillic: %r" % ( where, ru ) )
	if sorted( PLACEHOLDER_RE.findall( en ) ) != sorted( PLACEHOLDER_RE.findall( ru ) ):
		rep.error( "%s: placeholders differ: en=%r ru=%r" % ( where, en, ru ) )
	for word in LATIN_WORD_RE.findall( PLACEHOLDER_RE.sub( "", ru ) ):
		if word not in PROPER_NAMES:
			rep.error( "%s: Latin word %r in Russian value %r" % ( where, word, ru ) )


# ---------------------------------------------------------------------------------------
# SQL parsing / patching

def tokenize_values( vals ):
	"""Split an SQL VALUES list into (start, end, kind, value) tokens, keeping spans."""
	tokens = []
	i, n = 0, len( vals )
	while i < n:
		c = vals[i]
		if c in " ,":
			i += 1
			continue
		if c == "'":
			j = i + 1
			while j < n:
				if vals[j] == "'":
					if j + 1 < n and vals[j + 1] == "'":
						j += 2
						continue
					break
				j += 1
			if j >= n:
				raise ValueError( "unterminated string in VALUES" )
			tokens.append( ( i, j + 1, "str", sql_unescape( vals[i + 1:j] ) ) )
			i = j + 1
		else:
			j = i
			while j < n and vals[j] != ",":
				j += 1
			raw = vals[i:j].strip()
			tokens.append( ( i, j, "raw", raw ) )
			i = j
	return tokens


def read_sql_lines( path ):
	data = Path( path ).read_bytes()
	text = data.decode( "utf-8" )
	return text.splitlines( keepends=True )


def split_eol( line ):
	body = line.rstrip( "\r\n" )
	return body, line[len( body ):]


def patch_sql_lines( lines, translation, extra, rep, direction="ru" ):
	"""Return patched lines. direction 'ru' writes Russian; 'en' restores the English reference."""
	tr = { r["id"]: r for r in translation }
	seen = set()
	extra_index = collections.defaultdict( list )
	for r in extra:
		extra_index[r["table"]].append( r )
	extra_done = set()
	out = []
	for lineno, line in enumerate( lines, start=1 ):
		body, eol = split_eol( line )
		m = TRANSLATION_LINE_RE.match( body )
		if m:
			key, text = sql_unescape( m.group( 1 ) ), sql_unescape( m.group( 2 ) )
			row = tr.get( key )
			if row is None:
				rep.error( "SQL line %d: Translation key %s has no entry in %s" % ( lineno, key, TSV_PATH ) )
				out.append( line )
				continue
			if key in seen:
				rep.error( "SQL line %d: duplicate Translation key %s" % ( lineno, key ) )
			seen.add( key )
			want = row[direction]
			if text not in ( row["en"], row["ru"] ):
				rep.error( "SQL line %d: %s text %r matches neither en nor ru in the TSV" % ( lineno, key, text ) )
				out.append( line )
				continue
			new_body = body[:m.start( 2 )] + sql_escape( want ) + body[m.end( 2 ):]
			out.append( new_body + eol )
			continue
		m = INSERT_RE.match( body )
		if m and m.group( 1 ) in extra_index:
			table = m.group( 1 )
			cols = [ c.strip().strip( '"' ) for c in m.group( 2 ).split( "," ) ]
			vals_start = m.start( 3 )
			tokens = tokenize_values( m.group( 3 ) )
			if len( tokens ) != len( cols ):
				rep.error( "SQL line %d: cannot parse VALUES of %s" % ( lineno, table ) )
				out.append( line )
				continue
			new_body = body
			# apply from right to left so spans stay valid
			edits = []
			for r in extra_index[table]:
				if r["key_column"] not in cols or r["column"] not in cols:
					continue
				kt = tokens[cols.index( r["key_column"] )]
				if kt[3] != r["key"]:
					continue
				ct = tokens[cols.index( r["column"] )]
				if ct[2] != "str":
					rep.error( "SQL line %d: %s.%s for %s is not a string" % ( lineno, table, r["column"], r["key"] ) )
					continue
				if ct[3] not in ( r["en"], r["ru"] ):
					rep.error( "SQL line %d: %s.%s for %s is %r, matches neither en nor ru" % ( lineno, table, r["column"], r["key"], ct[3] ) )
					continue
				extra_done.add( ( table, r["key"], r["column"] ) )
				edits.append( ( vals_start + ct[0], vals_start + ct[1], "'" + sql_escape( r[direction] ) + "'" ) )
			for s, e, rep_text in sorted( edits, reverse=True ):
				new_body = new_body[:s] + rep_text + new_body[e:]
			out.append( new_body + eol )
			continue
		out.append( line )
	for key in tr:
		if key not in seen:
			rep.error( "TSV key %s not found in SQL" % key )
	for r in extra:
		if ( r["table"], r["key"], r["column"] ) not in extra_done:
			rep.error( "db_extra: %s/%s/%s not found in SQL" % ( r["table"], r["key"], r["column"] ) )
	return out


def naive_import( sql_text ):
	"""Execute the dump like DB::init: split on every ';', run each trimmed statement."""
	con = sqlite3.connect( ":memory:" )
	errors = []
	for idx, stmt in enumerate( sql_text.split( ";" ) ):
		s = stmt.strip()
		if not s:
			continue
		try:
			con.execute( s )
		except sqlite3.Error as e:
			errors.append( ( idx, str( e ), s[:80] ) )
	return con, errors


def table_counts( con ):
	names = [ r[0] for r in con.execute( "SELECT name FROM sqlite_master WHERE type='table'" ) ]
	return { n: con.execute( 'SELECT COUNT(*) FROM "%s"' % n.replace( '"', '""' ) ).fetchone()[0] for n in names }


# ---------------------------------------------------------------------------------------
# Fonts (minimal sfnt reader: name + cmap tables)

def _sfnt_tables( data ):
	if data[:4] not in ( b"\x00\x01\x00\x00", b"OTTO", b"true" ):
		raise ValueError( "not a TrueType/OpenType font" )
	num = struct.unpack( ">H", data[4:6] )[0]
	tables = {}
	for i in range( num ):
		tag, _, off, length = struct.unpack( ">4sIII", data[12 + 16 * i:28 + 16 * i] )
		tables[tag.decode( "latin-1" )] = ( off, length )
	return tables


def font_families( data ):
	tables = _sfnt_tables( data )
	off, _ = tables["name"]
	_, count, str_off = struct.unpack( ">HHH", data[off:off + 6] )
	names = set()
	for i in range( count ):
		pid, eid, lid, nid, length, noff = struct.unpack( ">HHHHHH", data[off + 6 + 12 * i:off + 18 + 12 * i] )
		if nid not in ( 1, 16, 21 ):
			continue
		raw = data[off + str_off + noff:off + str_off + noff + length]
		try:
			names.add( raw.decode( "utf-16-be" ) if pid in ( 0, 3 ) else raw.decode( "mac_roman" ) )
		except UnicodeDecodeError:
			pass
	return names


def font_codepoints( data ):
	tables = _sfnt_tables( data )
	off, _ = tables["cmap"]
	_, num = struct.unpack( ">HH", data[off:off + 4] )
	subtables = []
	for i in range( num ):
		pid, eid, soff = struct.unpack( ">HHI", data[off + 4 + 8 * i:off + 12 + 8 * i] )
		subtables.append( ( pid, eid, off + soff ) )
	covered = set()
	for pid, eid, p in subtables:
		fmt = struct.unpack( ">H", data[p:p + 2] )[0]
		if fmt == 4:
			segx2 = struct.unpack( ">H", data[p + 6:p + 8] )[0]
			seg = segx2 // 2
			ends = struct.unpack( ">%dH" % seg, data[p + 14:p + 14 + segx2] )
			starts = struct.unpack( ">%dH" % seg, data[p + 16 + segx2:p + 16 + 2 * segx2] )
			deltas = struct.unpack( ">%dh" % seg, data[p + 16 + 2 * segx2:p + 16 + 3 * segx2] )
			ro_pos = p + 16 + 3 * segx2
			ranges = struct.unpack( ">%dH" % seg, data[ro_pos:ro_pos + segx2] )
			for i in range( seg ):
				for c in range( starts[i], ends[i] + 1 ):
					if c == 0xFFFF:
						continue
					if ranges[i] == 0:
						glyph = ( c + deltas[i] ) & 0xFFFF
					else:
						addr = ro_pos + 2 * i + ranges[i] + 2 * ( c - starts[i] )
						glyph = struct.unpack( ">H", data[addr:addr + 2] )[0]
						if glyph:
							glyph = ( glyph + deltas[i] ) & 0xFFFF
					if glyph:
						covered.add( c )
		elif fmt == 12:
			ngroups = struct.unpack( ">I", data[p + 12:p + 16] )[0]
			for g in range( ngroups ):
				s, e, sg = struct.unpack( ">III", data[p + 16 + 12 * g:p + 28 + 12 * g] )
				for c in range( s, e + 1 ):
					if sg + ( c - s ):
						covered.add( c )
	return covered


def font_index( font_dir ):
	"""Map family name -> list of (path, codepoints) for fonts in one directory."""
	index = collections.defaultdict( list )
	if not font_dir.is_dir():
		return index
	for f in sorted( font_dir.iterdir() ):
		if f.suffix.lower() not in ( ".ttf", ".otf" ):
			continue
		try:
			data = f.read_bytes()
			cps = font_codepoints( data )
			for fam in font_families( data ):
				index[fam].append( ( f, cps ) )
		except ( ValueError, KeyError, struct.error ) as e:
			index["<unreadable:%s>" % f.name].append( ( f, set() ) )
	return index


def font_references( xaml_root ):
	"""Yield (xaml file, base dir, raw value) for every FontFamily reference."""
	for f in sorted( xaml_root.rglob( "*.xaml" ) ):
		try:
			tree = ET.parse( f )
		except ET.ParseError:
			continue
		for el in tree.iter():
			val = el.attrib.get( "FontFamily" )
			if val:
				yield f, val
			if el.tag.endswith( "}FontFamily" ) or el.tag == "FontFamily":
				if el.text and el.text.strip():
					yield f, el.text.strip()


def check_fonts( rep, xaml_root, required_chars, missing_is_error ):
	for f, value in font_references( xaml_root ):
		for part in value.split( "," ):
			part = part.strip()
			if part.startswith( "{" ) or "#" not in part:
				continue
			rel, family = part.split( "#", 1 )
			font_dir = ( f.parent / rel ).resolve() if rel else f.parent
			idx = font_index( font_dir )
			where = "%s FontFamily=%r" % ( f.relative_to( xaml_root ), part )
			if family not in idx:
				msg = "%s: family %r not found in %s (found: %s)" % ( where, family, font_dir, ", ".join( sorted( idx ) ) or "no fonts" )
				( rep.error if missing_is_error else rep.warn )( msg )
				continue
			cps = set().union( *[ c for _, c in idx[family] ] )
			missing = sorted( ch for ch in required_chars if ord( ch ) not in cps )
			files = ", ".join( p.name for p, _ in idx[family] )
			if family.startswith( "8-bit" ) or family.startswith( "Hermeneus" ):
				rep.warn( "%s: Latin-only font still referenced" % where )
				continue
			if missing:
				rep.error( "%s (%s): missing glyphs %s" % ( where, files, "".join( missing ) ) )
			else:
				rep.note( "%s -> %s covers all %d Russian UI characters" % ( where, files, len( required_chars ) ) )


# ---------------------------------------------------------------------------------------
# XAML checks

X_NS = "{http://schemas.microsoft.com/winfx/2006/xaml}"
DISPLAY_ATTRS = ( "Content", "Text", "Header", "ToolTip", "Title" )
DISPLAY_PROPS = { "Content", "Text", "Header", "ToolTip", "Title" }


def xaml_strings( path ):
	tree = ET.parse( path )
	out = collections.OrderedDict()
	for el in tree.iter():
		key = el.attrib.get( X_NS + "Key" )
		if key is not None and el.tag.endswith( "}String" ):
			out[key] = el.text or ""
	return out


def active_xaml_files( root ):
	xr = root / XAML_DIR
	for f in sorted( xr.rglob( "*.xaml" ) ):
		rel = f.relative_to( xr ).as_posix()
		if rel.startswith( ( "Theme/", "localization/", "SampleData/" ) ):
			continue
		yield f


def english_leftovers( path ):
	"""Return (where, text) for display text with Latin words outside the proper-name list."""
	found = []
	tree = ET.parse( path )

	def bad( text ):
		if not text or text.strip().startswith( "{" ):
			return False
		words = [ w for w in LATIN_WORD_RE.findall( text ) if w not in PROPER_NAMES ]
		return bool( words )

	for el in tree.iter():
		tag = el.tag.split( "}" )[-1]
		for a in DISPLAY_ATTRS:
			if bad( el.attrib.get( a ) ):
				found.append( ( "%s@%s" % ( tag, a ), el.attrib[a] ) )
		if tag == "Setter" and el.attrib.get( "Property" ) in DISPLAY_PROPS and bad( el.attrib.get( "Value" ) ):
			found.append( ( "Setter %s" % el.attrib.get( "Property" ), el.attrib["Value"] ) )
		# typed color data (<Color>#AARRGGBB</Color>) is not UI text; anything else in a
		# Color element, and all tails, are still checked
		typed_color = tag == "Color" and el.text and HEX_COLOR_RE.match( el.text.strip() )
		if tag not in ( "Geometry", "FontFamily", "String" ) and not typed_color:
			for t in ( el.text, ):
				if t and t.strip() and bad( t ):
					found.append( ( "%s text" % tag, t.strip() ) )
		if el.tail and el.tail.strip() and bad( el.tail ):
			found.append( ( "%s tail" % tag, el.tail.strip() ) )
	return found


def check_xaml( root, rep ):
	xr = root / XAML_DIR
	parsed = 0
	for f in sorted( xr.rglob( "*.xaml" ) ):
		try:
			ET.parse( f )
			parsed += 1
		except ET.ParseError as e:
			rep.error( "XML parse error: %s: %s" % ( f.relative_to( root ), e ) )
	rep.note( "%d XAML files parse as XML" % parsed )

	en = xaml_strings( root / EN_XAML )
	ru = xaml_strings( root / RU_XAML )
	if len( en ) != XAML_KEYS:
		rep.error( "en_US.xaml has %d keys, expected %d" % ( len( en ), XAML_KEYS ) )
	if set( en ) != set( ru ):
		rep.error( "ru_RU.xaml keys differ from en_US.xaml: missing %s, extra %s" % (
			sorted( set( en ) - set( ru ) ), sorted( set( ru ) - set( en ) ) ) )
	for k, v in ru.items():
		if k not in en:
			continue
		if k.endswith( "_" ):
			if v != en[k]:
				rep.warn( "section marker %s changed" % k )
			continue
		where = "ru_RU.xaml %s" % k
		if not v.strip():
			rep.error( "%s: empty" % where )
		elif k not in XAML_LATIN_OK:
			if not CYRILLIC_RE.search( v ):
				rep.error( "%s: no Cyrillic in %r" % ( where, v ) )
			for w in LATIN_WORD_RE.findall( PLACEHOLDER_RE.sub( "", v ) ):
				if w not in PROPER_NAMES:
					rep.error( "%s: Latin word %r" % ( where, w ) )
		if sorted( PLACEHOLDER_RE.findall( en[k] ) ) != sorted( PLACEHOLDER_RE.findall( v ) ):
			rep.error( "%s: placeholders differ" % where )
	rep.note( "ru_RU.xaml: %d keys checked against en_US.xaml" % len( ru ) )

	pages = 0
	for f in active_xaml_files( root ):
		raw = f.read_text( encoding="utf-8" )
		no_comments = re.sub( r"<!--.*?-->", "", raw, flags=re.S )
		if "localization/en_US.xaml" in no_comments or "localization/fr_FR.xaml" in no_comments:
			rep.error( "%s still merges a non-Russian string dictionary" % f.relative_to( root ) )
		if "localization/ru_RU.xaml" in no_comments:
			pages += 1
		try:
			for where, text in english_leftovers( f ):
				rep.error( "English UI text in %s (%s): %r" % ( f.relative_to( root ), where, text ) )
		except ET.ParseError:
			pass
	rep.note( "%d active pages merge localization/ru_RU.xaml" % pages )
	return ru


# ---------------------------------------------------------------------------------------
# Commands

def russian_chars( translation, extra, ru_xaml, root ):
	chars = set()
	texts = [ r["ru"] for r in translation ] + [ r["ru"] for r in extra ] + list( ru_xaml.values() )
	for f in active_xaml_files( root ):
		texts.append( f.read_text( encoding="utf-8" ) )
	for t in texts:
		for ch in t:
			if ord( ch ) > 0x7F and ( CYRILLIC_RE.match( ch ) or ch in "«»—–×…" ):
				chars.add( ch )
	return chars


def cmd_apply_sql( args ):
	root = Path( args.repo )
	rep = Report()
	translation = load_translation( root )
	extra = load_extra( root )
	src = Path( args.sql ) if args.sql else root / SQL_PATH
	lines = read_sql_lines( src )
	out = patch_sql_lines( lines, translation, extra, rep, direction=args.direction )
	if not rep.errors:
		text = "".join( out )
		_, errs = naive_import( text )
		if errs:
			for idx, msg, head in errs[:20]:
				rep.warn( "naive import statement %d: %s (%s)" % ( idx, msg, head ) )
		dest = Path( args.out ) if args.out else src
		if "".join( lines ) == text:
			rep.note( "%s already %s, unchanged" % ( src, "localized" if args.direction == "ru" else "English" ) )
		else:
			dest.parent.mkdir( parents=True, exist_ok=True )
			with open( dest, "w", encoding="utf-8", newline="" ) as f:
				f.write( text )
			rep.note( "wrote %s (%s)" % ( dest, args.direction ) )
	return rep.emit( "apply-sql" )


def cmd_check( args ):
	root = Path( args.repo )
	rep = Report()

	translation = load_translation( root )
	extra = load_extra( root )
	ids = [ r["id"] for r in translation ]
	if len( ids ) != TRANSLATION_ROWS:
		rep.error( "translation.tsv has %d rows, expected %d" % ( len( ids ), TRANSLATION_ROWS ) )
	dups = [ k for k, c in collections.Counter( ids ).items() if c > 1 ]
	if dups:
		rep.error( "duplicate ids in translation.tsv: %s" % dups )
	for r in translation:
		check_value_text( rep, "translation.tsv:%d %s" % ( r["_line"], r["id"] ), r["en"], r["ru"] )
	for r in extra:
		check_value_text( rep, "db_extra.tsv:%d %s/%s/%s" % ( r["_line"], r["table"], r["key"], r["column"] ), r["en"], r["ru"] )
	rep.note( "translation.tsv: %d rows, db_extra.tsv: %d cells" % ( len( translation ), len( extra ) ) )

	# SQL state
	sql_file = root / SQL_PATH
	lines = read_sql_lines( sql_file )
	sub = Report()
	localized = patch_sql_lines( lines, translation, extra, sub, direction="ru" )
	rep.errors += sub.errors
	if not sub.errors:
		if localized != lines:
			rep.error( "%s is not fully localized; run: python3 tools/l10n/ru_locale.py apply-sql --repo ." % SQL_PATH )
		else:
			rep.note( "%s holds the Russian texts for all keys" % SQL_PATH )
	sql_text = "".join( lines )
	con, errs = naive_import( sql_text )
	ref_errs = None
	if args.reference_sql:
		ref_lines = read_sql_lines( args.reference_sql )
		ref_con, ref_errs = naive_import( "".join( ref_lines ) )
		new = []
		left = collections.Counter( m for _, m, _ in ref_errs )
		for e in errs:
			if left[e[1]] > 0:
				left[e[1]] -= 1
			else:
				new.append( e )
		for idx, msg, head in new:
			rep.error( "naive import: statement %d fails only in the localized SQL: %s (%s)" % ( idx, msg, head ) )
		if table_counts( con ) != table_counts( ref_con ):
			rep.error( "row counts per table differ from the reference SQL" )
		else:
			rep.note( "naive ';' import: same tables and row counts as reference (%d tables)" % len( table_counts( con ) ) )
		# every non-localized line must be identical
		ref_restored = patch_sql_lines( ref_lines, translation, extra, Report(), direction="ru" )
		cur = [ split_eol( l )[0] for l in lines ]
		exp = [ split_eol( l )[0] for l in ref_restored ]
		if cur != exp:
			diff = next( ( i for i, ( a, b ) in enumerate( zip( cur, exp ) ) if a != b ), min( len( cur ), len( exp ) ) )
			rep.error( "SQL differs from reference beyond localized cells (first difference at line %d)" % ( diff + 1 ) )
		else:
			rep.note( "SQL equals reference with only Translation/db_extra text cells replaced" )
	else:
		for idx, msg, head in errs:
			rep.error( "naive import: statement %d: %s (%s)" % ( idx, msg, head ) )
		if not errs:
			rep.note( "naive ';' import of %s: no errors, %d tables" % ( SQL_PATH, len( table_counts( con ) ) ) )
		rep.warn( "no --reference-sql given: gameplay rows were not compared with upstream" )

	ru_xaml = check_xaml( root, rep )

	# JSON data
	for f in sorted( ( root / "content/JSON" ).glob( "*.json" ) ):
		if f.name in INACTIVE_JSON:
			continue
		try:
			json.loads( f.read_text( encoding="utf-8" ) )
		except ValueError as e:
			rep.error( "JSON parse error %s: %s" % ( f.relative_to( root ), e ) )
	cfg = json.loads( ( root / CONFIG_JSON ).read_text( encoding="utf-8" ) )
	if cfg.get( "language" ) != "ru_RU":
		rep.error( "config.json language is %r, expected 'ru_RU'" % cfg.get( "language" ) )
	if int( cfg.get( "WindowWidth", 0 ) ) > 1920 or int( cfg.get( "WindowHeight", 0 ) ) > 1080 or int( cfg.get( "WindowPosX", 0 ) ) > 400:
		rep.error( "config.json default window does not fit a 1920x1080 screen" )
	else:
		rep.note( "config.json: ru_RU, %sx%s at %s,%s" % ( cfg.get( "WindowWidth" ), cfg.get( "WindowHeight" ), cfg.get( "WindowPosX" ), cfg.get( "WindowPosY" ) ) )

	chars = russian_chars( translation, extra, ru_xaml, root )
	check_fonts( rep, root / XAML_DIR, chars, missing_is_error=not args.allow_missing_theme_fonts )
	return rep.emit( "check" )


def find_one( base, patterns ):
	for p in patterns:
		hits = sorted( base.glob( p ) )
		if hits:
			return hits[0]
	return None


def cmd_check_package( args ):
	pkg = Path( args.dir )
	rep = Report()
	required = {
		"game executable": [ "Ingnomia.exe" ],
		"Qt core": [ "Qt6Core*.dll" ],
		"Qt Windows platform plugin": [ "platforms/qwindows*.dll" ],
		"NoesisGUI runtime": [ "Noesis*.dll" ],
		"Steam API": [ "steam_api64.dll" ],
		"OpenAL": [ "OpenAL32.dll", "soft_oal.dll", "*OpenAL*.dll", "*openal*.dll" ],
		"database": [ "content/db/ingnomia.db.sql" ],
		"Russian strings": [ "content/xaml/localization/ru_RU.xaml" ],
		"tilesheets": [ "content/tilesheet/*.png" ],
		"audio": [ "content/audio/**/*.wav", "content/audio/**/*.ogg" ],
		"Cyrillic UI font": [ "content/xaml/Fonts/weblysleekuisb.ttf" ],
		"theme font": [ "content/xaml/Theme/Fonts/*.otf" ],
		"Russian readme": [ "ПРОЧТИ МЕНЯ.txt" ],
		"launcher": [ "Запустить Ingnomia.cmd" ],
		"provenance": [ "provenance.json" ],
		"AGPL license": [ "LICENSE", "LICENSE.txt" ],
	}
	for what, pats in required.items():
		hit = find_one( pkg, pats )
		if hit is None:
			rep.error( "missing %s (%s)" % ( what, " | ".join( pats ) ) )
		else:
			rep.note( "%s: %s" % ( what, hit.relative_to( pkg ).as_posix() ) )
	tiles = list( pkg.glob( "content/tilesheet/*.png" ) )
	if 0 < len( tiles ) < 10:
		rep.error( "only %d tilesheet PNGs, the game needs the full set" % len( tiles ) )

	sql = pkg / "content/db/ingnomia.db.sql"
	if args.repo and sql.is_file():
		root = Path( args.repo )
		translation, extra = load_translation( root ), load_extra( root )
		lines = read_sql_lines( sql )
		sub = Report()
		if patch_sql_lines( lines, translation, extra, sub, "ru" ) != lines or sub.errors:
			rep.error( "packaged SQL is not the fully localized database" )
		else:
			rep.note( "packaged SQL holds all Russian texts" )
		_, errs = naive_import( "".join( lines ) )
		ref = Path( args.reference_sql ) if args.reference_sql else None
		ref_count = len( naive_import( "".join( read_sql_lines( ref ) ) )[1] ) if ref else 0
		if len( errs ) > ref_count:
			rep.error( "packaged SQL: %d naive-import errors (reference: %d)" % ( len( errs ), ref_count ) )
		ru = xaml_strings( pkg / "content/xaml/localization/ru_RU.xaml" ) if ( pkg / "content/xaml/localization/ru_RU.xaml" ).is_file() else {}
		chars = set()
		for t in [ r["ru"] for r in translation ] + list( ru.values() ):
			chars.update( ch for ch in t if CYRILLIC_RE.match( ch ) or ch in "«»—–×…" )
		check_fonts( rep, pkg / "content/xaml", chars, missing_is_error=True )
	prov = pkg / "provenance.json"
	if prov.is_file():
		try:
			p = json.loads( prov.read_text( encoding="utf-8" ) )
			rep.note( "provenance mode: %s, C++ fixes included: %s" % ( p.get( "mode" ), p.get( "source_cpp_changes_included" ) ) )
		except ValueError:
			rep.error( "provenance.json is not valid JSON" )
	return rep.emit( "check-package %s" % pkg )


CPP_SKIP_RE = re.compile( r"qDebug|qInfo|qWarning|qCritical|NS_LOG|NS_CHECK|OnPropertyChanged|NsProp|NS_IMPLEMENT|"
	r"LoadComponent|FindName|DB::|selectRow|\.value\(|\.insert\(|contains\(|Global::cfg|logger\(\)|\blog\(|m_log|"
	r"\"Visible\"|\"Hidden\"|\"Collapsed\"|#include|m_currentAction|signalStatus|sendOverlayMessage" )
CPP_SCOPE = [ "src/gui/**/*.cpp", "src/game/game.cpp", "src/game/newgamesettings.cpp", "src/game/plant.cpp",
	"src/game/item.cpp", "src/game/workshop.cpp", "src/game/stockpile.cpp", "src/game/farm.cpp",
	"src/game/grove.cpp", "src/game/pasture.cpp", "src/game/room.cpp", "src/game/militarymanager.cpp",
	"src/game/eventmanager.cpp", "src/base/util.cpp" ]
CPP_IGNORE_FILES = { "openalwrapper.cpp", "mainwindowrenderer.cpp", "keybindings.cpp", "aggregatorsound.cpp" }
STRING_RE = re.compile( r'"((?:[^"\\]|\\.)*)"' )


def cmd_scan_cpp( args ):
	root = Path( args.repo )
	rep = Report()
	files = set()
	for pat in CPP_SCOPE:
		files.update( root.glob( pat ) )
	count = 0
	for f in sorted( files ):
		if f.name in CPP_IGNORE_FILES:
			continue
		for n, line in enumerate( f.read_text( encoding="utf-8", errors="replace" ).splitlines(), start=1 ):
			s = line.strip()
			if s.startswith( ( "//", "*", "/*", "#" ) ) or CPP_SKIP_RE.search( line ):
				continue
			code = line.split( "//" )[0]
			for lit in STRING_RE.findall( code ):
				if CYRILLIC_RE.search( lit ) or not re.search( r"[A-Za-z]{3,}", lit ):
					continue
				if " " in lit.strip() or re.search( r"[.!?:]\s*$", lit ):
					count += 1
					( rep.error if args.strict else rep.warn )( "%s:%d: %r" % ( f.relative_to( root ).as_posix(), n, lit ) )
	rep.note( "heuristic scan finished, %d candidate English display literal(s)" % count )
	return rep.emit( "scan-cpp" )


def main():
	ap = argparse.ArgumentParser( description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter )
	sub = ap.add_subparsers( dest="cmd", required=True )

	a = sub.add_parser( "apply-sql" )
	a.add_argument( "--repo", default="." )
	a.add_argument( "--sql", help="input SQL (default: repo content/db/ingnomia.db.sql, patched in place)" )
	a.add_argument( "--out", help="output path (default: overwrite input)" )
	a.add_argument( "--direction", choices=( "ru", "en" ), default="ru" )
	a.set_defaults( func=cmd_apply_sql )

	c = sub.add_parser( "check" )
	c.add_argument( "--repo", default="." )
	c.add_argument( "--reference-sql", help="pristine upstream ingnomia.db.sql (v0.9.0) for gameplay-row comparison" )
	c.add_argument( "--allow-missing-theme-fonts", action="store_true",
		help="report missing font files as warnings (source checkouts without release assets)" )
	c.set_defaults( func=cmd_check )

	p = sub.add_parser( "check-package" )
	p.add_argument( "--dir", required=True )
	p.add_argument( "--repo", default="." )
	p.add_argument( "--reference-sql" )
	p.set_defaults( func=cmd_check_package )

	s = sub.add_parser( "scan-cpp" )
	s.add_argument( "--repo", default="." )
	s.add_argument( "--strict", action="store_true", help="treat candidates as errors" )
	s.set_defaults( func=cmd_scan_cpp )

	args = ap.parse_args()
	try:
		return args.func( args )
	except ( ValueError, OSError ) as e:
		print( "ERROR " + str( e ) )
		return 2


if __name__ == "__main__":
	sys.exit( main() )
