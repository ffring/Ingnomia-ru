#!/usr/bin/env python3
"""Build the Russian Windows test package (Python 3.8+, standard library only).

Modes
  install   Preferred. Package a CMake install tree of this fork (rebuilt Ingnomia.exe with
            the C++ locale fixes). Missing release-only assets (tilesheets, audio, theme
            fonts) can be filled from the pinned official 0.9.0 ZIP with --asset-zip.
  release   Quick data-only package: the unmodified official 0.9.0 Ingnomia.exe with this
            repository's localized SQL, XAML and JSON copied over it. C++ fixes are absent.

Every input ZIP is checked against packaging/windows/upstream-release.json (size + SHA-256).
The output ZIP is deterministic for identical inputs and --epoch.

Examples
  python3 tools/package/make_windows_package.py --mode release --release-zip /path/Ingnomia-0.9.0-windows-x64.zip --out dist
  python3 tools/package/make_windows_package.py --mode install --install-dir install --asset-zip /path/Ingnomia-0.9.0-windows-x64.zip --out dist --noesis-evaluation
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path, PurePosixPath

REPO = Path( __file__ ).resolve().parents[2]
sys.path.insert( 0, str( REPO / "tools" / "l10n" ) )
import ru_locale  # noqa: E402

PIN = REPO / "packaging/windows/upstream-release.json"
TEMPLATES = REPO / "packaging/windows"
PKG_ROOT = "Ingnomia-ru"
LAUNCHER = "Запустить Ingnomia.cmd"
README = "ПРОЧТИ МЕНЯ.txt"
FORK_URL = "https://github.com/ffring/Ingnomia-ru"
# Content that only ships with the official release (not tracked in the source tree).
RELEASE_ASSET_DIRS = ( "content/tilesheet/", "content/audio/", "content/xaml/Theme/Fonts/" )


def sha256_file( path ):
	h = hashlib.sha256()
	with open( path, "rb" ) as f:
		for chunk in iter( lambda: f.read( 1 << 20 ), b"" ):
			h.update( chunk )
	return h.hexdigest()


def verify_pinned_zip( path ):
	pin = json.loads( PIN.read_text( encoding="utf-8" ) )
	size = path.stat().st_size
	digest = sha256_file( path )
	if size != pin["size"] or digest != pin["sha256"]:
		raise SystemExit( "ERROR %s does not match the pinned %s (size %d/%d, sha256 %s/%s)" % (
			path, pin["name"], size, pin["size"], digest, pin["sha256"] ) )
	print( "ok    %s matches pinned %s" % ( path.name, pin["name"] ) )
	return pin, digest


def safe_members( zf ):
	"""Yield (info, posix path) for regular files, rejecting absolute or '..' paths."""
	for info in zf.infolist():
		name = info.filename.replace( "\\", "/" )
		p = PurePosixPath( name )
		if p.is_absolute() or ".." in p.parts or ( p.parts and ":" in p.parts[0] ):
			raise SystemExit( "ERROR unsafe path in ZIP: %r" % info.filename )
		if not name.endswith( "/" ):
			yield info, p


def release_root( zf ):
	"""Directory prefix inside the official ZIP that holds Ingnomia.exe."""
	for info, p in safe_members( zf ):
		if p.name.lower() == "ingnomia.exe":
			return PurePosixPath( *p.parts[:-1] )
	raise SystemExit( "ERROR Ingnomia.exe not found in release ZIP" )


def extract_tree( zf, prefix, dest, only=None, skip_existing=False ):
	added = []
	for info, p in safe_members( zf ):
		try:
			rel = p.relative_to( prefix ) if prefix.parts else p
		except ValueError:
			continue
		rel_s = rel.as_posix()
		if only and not rel_s.startswith( only ):
			continue
		target = dest / rel
		if skip_existing and target.exists():
			continue
		target.parent.mkdir( parents=True, exist_ok=True )
		with zf.open( info ) as src, open( target, "wb" ) as dst:
			shutil.copyfileobj( src, dst )
		added.append( rel_s )
	return added


def write_text( path, text, crlf=True, bom=False ):
	if crlf:
		text = text.replace( "\r\n", "\n" ).replace( "\n", "\r\n" )
	data = text.encode( "utf-8" )
	if bom:
		data = b"\xef\xbb\xbf" + data
	path.write_bytes( data )


def overlay_repo_content( stage ):
	"""Copy localized data files from the repository over the release content."""
	files = []
	for f in sorted( ( REPO / "content/xaml" ).rglob( "*.xaml" ) ):
		files.append( f.relative_to( REPO ) )
	for f in sorted( ( REPO / "content/JSON" ).glob( "*.json" ) ):
		files.append( f.relative_to( REPO ) )
	files.append( Path( "content/db/ingnomia.db.sql" ) )
	out = []
	for rel in files:
		target = stage / rel
		existed = target.exists()
		target.parent.mkdir( parents=True, exist_ok=True )
		shutil.copyfile( REPO / rel, target )
		out.append( { "path": rel.as_posix(), "sha256": sha256_file( target ), "replaced_release_file": existed } )
	return out


def check_release_sql_matches_repo( stage ):
	"""The release SQL must turn into exactly the repository SQL when localized."""
	rep = ru_locale.Report()
	translation = ru_locale.load_translation( REPO )
	extra = ru_locale.load_extra( REPO )
	rel_lines = ru_locale.read_sql_lines( stage / "content/db/ingnomia.db.sql" )
	localized = ru_locale.patch_sql_lines( rel_lines, translation, extra, rep, "ru" )
	repo_lines = ru_locale.read_sql_lines( REPO / "content/db/ingnomia.db.sql" )
	strip = lambda ls: [ ru_locale.split_eol( l )[0] for l in ls ]
	if rep.errors or strip( localized ) != strip( repo_lines ):
		for e in rep.errors[:10]:
			print( "ERROR " + e )
		raise SystemExit( "ERROR release SQL + translation.tsv != repository SQL; the repository data is not "
			"compatible with the unmodified 0.9.0 executable (or the repository SQL is not localized yet)" )
	print( "ok    release SQL localizes to exactly the repository SQL" )


def build_readme( mode, evaluation ):
	parts = [ "README.common.ru.txt", "README.install.ru.txt" if mode == "install" else "README.release.ru.txt" ]
	if evaluation:
		parts.append( "README.evaluation.ru.txt" )
	parts.append( "README.license.ru.txt" )
	return "".join( ( TEMPLATES / p ).read_text( encoding="utf-8" ) for p in parts )


def deterministic_zip( stage_root, zip_path, epoch ):
	t = time.gmtime( max( epoch, 315532800 ) )
	date_time = ( t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec )
	files = sorted( p for p in stage_root.rglob( "*" ) if p.is_file() )
	with zipfile.ZipFile( zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9 ) as zf:
		for f in files:
			arc = ( PurePosixPath( stage_root.name ) / f.relative_to( stage_root ).as_posix() ).as_posix()
			info = zipfile.ZipInfo( arc, date_time )
			info.compress_type = zipfile.ZIP_DEFLATED
			info.external_attr = ( 0o644 & 0xFFFF ) << 16
			info.flag_bits |= 0x800  # UTF-8 file names (Cyrillic launcher/readme)
			with open( f, "rb" ) as src:
				zf.writestr( info, src.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9 )
	return len( files )


def main():
	ap = argparse.ArgumentParser( description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter )
	ap.add_argument( "--mode", choices=( "install", "release" ), required=True )
	ap.add_argument( "--install-dir", help="CMake install tree (mode install)" )
	ap.add_argument( "--release-zip", help="pinned official Ingnomia-0.9.0-windows-x64.zip (mode release)" )
	ap.add_argument( "--asset-zip", help="pinned official ZIP used only to fill missing release assets (mode install)" )
	ap.add_argument( "--noesis-evaluation", action="store_true", help="the install tree was built with INGNOMIA_NOESIS_EVALUATION=ON" )
	ap.add_argument( "--source-rev", default=os.environ.get( "GITHUB_SHA", "unknown" ), help="fork commit the package was built from" )
	ap.add_argument( "--epoch", type=int, default=int( os.environ.get( "SOURCE_DATE_EPOCH", "315532800" ) ),
		help="timestamp for ZIP entries (default SOURCE_DATE_EPOCH or 1980-01-01)" )
	ap.add_argument( "--out", required=True, help="output directory" )
	args = ap.parse_args()

	out = Path( args.out ).resolve()
	out.mkdir( parents=True, exist_ok=True )
	work = out / ( "staging-" + args.mode )
	if work.exists():
		shutil.rmtree( work )
	stage = work / PKG_ROOT
	stage.mkdir( parents=True )

	inputs = []
	pin = None
	overlay = []
	assets_added = []

	if args.mode == "release":
		if not args.release_zip:
			raise SystemExit( "ERROR --release-zip is required for --mode release" )
		zpath = Path( args.release_zip ).resolve()
		pin, digest = verify_pinned_zip( zpath )
		inputs.append( { "role": "official release (executable, runtimes, assets)", "name": pin["name"], "url": pin["url"], "size": pin["size"], "sha256": digest } )
		with zipfile.ZipFile( zpath ) as zf:
			prefix = release_root( zf )
			extract_tree( zf, prefix, stage )
		check_release_sql_matches_repo( stage )
		overlay = overlay_repo_content( stage )
		cpp_included = False
		package_name = "Ingnomia-ru-0.9.0-windows-x64-data-only"
	else:
		if not args.install_dir:
			raise SystemExit( "ERROR --install-dir is required for --mode install" )
		src = Path( args.install_dir ).resolve()
		if not ( src / "Ingnomia.exe" ).is_file():
			raise SystemExit( "ERROR %s has no Ingnomia.exe (expected a CMake install tree with CMAKE_INSTALL_BINDIR=.)" % src )
		shutil.copytree( src, stage, dirs_exist_ok=True )
		for junk in list( stage.rglob( "*.iobj" ) ) + list( stage.rglob( "*.ipdb" ) ):
			junk.unlink()
		inputs.append( { "role": "CMake install tree of the fork", "path": str( src ), "fork_revision": args.source_rev } )
		if args.asset_zip:
			zpath = Path( args.asset_zip ).resolve()
			pin, digest = verify_pinned_zip( zpath )
			inputs.append( { "role": "official release, only for missing release assets", "name": pin["name"], "url": pin["url"], "size": pin["size"], "sha256": digest } )
			with zipfile.ZipFile( zpath ) as zf:
				prefix = release_root( zf )
				for d in RELEASE_ASSET_DIRS:
					assets_added += extract_tree( zf, prefix, stage, only=d, skip_existing=True )
			print( "ok    %d release asset file(s) added to the install tree" % len( assets_added ) )
		cpp_included = True
		package_name = "Ingnomia-ru-0.9.0-windows-x64-build"

	if not ( stage / "LICENSE" ).exists():
		shutil.copyfile( REPO / "LICENSE", stage / "LICENSE" )
	write_text( stage / README, build_readme( args.mode, args.noesis_evaluation ), crlf=True, bom=True )
	write_text( stage / LAUNCHER, ( TEMPLATES / "launcher.cmd" ).read_text( encoding="utf-8" ), crlf=True, bom=False )

	files = sorted( p for p in stage.rglob( "*" ) if p.is_file() )
	provenance = {
		"package": package_name + ".zip",
		"mode": args.mode,
		"source_cpp_changes_included": cpp_included,
		"noesis_evaluation_build": bool( args.noesis_evaluation ) if args.mode == "install" else None,
		"executable": "rebuilt from the fork" if cpp_included else "unmodified official Ingnomia 0.9.0",
		"fork_repository": FORK_URL,
		"fork_revision": args.source_rev,
		"upstream_release": pin,
		"inputs": inputs,
		"localized_overlay": overlay,
		"release_assets_added": assets_added,
		"networking": "single-player only; no network or co-op mode",
		"generator": "tools/package/make_windows_package.py",
		"files": [ { "path": p.relative_to( stage ).as_posix(), "size": p.stat().st_size, "sha256": sha256_file( p ) }
			for p in files ],
	}
	( stage / "provenance.json" ).write_text( json.dumps( provenance, ensure_ascii=False, indent=2, sort_keys=False ) + "\n", encoding="utf-8" )

	# validate the staged package with the same checks as `ru_locale.py check-package`
	ns = argparse.Namespace( dir=str( stage ), repo=str( REPO ), reference_sql=None )
	if ru_locale.cmd_check_package( ns ) != 0:
		raise SystemExit( "ERROR package validation failed; ZIP not written" )

	zip_path = out / ( package_name + ".zip" )
	count = deterministic_zip( stage, zip_path, args.epoch )
	digest = sha256_file( zip_path )
	( out / ( package_name + ".zip.sha256" ) ).write_text( "%s  %s\n" % ( digest, zip_path.name ), encoding="utf-8" )
	print( "ok    wrote %s (%d files, sha256 %s)" % ( zip_path, count, digest ) )
	return 0


if __name__ == "__main__":
	sys.exit( main() )
