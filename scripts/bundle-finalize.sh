#!/bin/bash
# One-shot finalizer for a macdeployqt-produced .app bundle.
# Copies all remaining Homebrew dylib dependencies, rewrites absolute install
# names to @rpath (exactly once per file), then signs bottom-up.
# Usage: bundle-finalize.sh <App.app>
set -uo pipefail

APP="${1:?usage: bundle-finalize.sh <App.app>}"
FW="$APP/Contents/Frameworks"
EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
EXE="$APP/Contents/MacOS/$EXE_NAME"
mkdir -p "$FW"

# Convert an absolute dependency path to @rpath form, preserving framework layout.
to_rpath() {
    local p="$1"
    if [[ "$p" =~ ([^/]+\.framework/.*)$ ]]; then
        printf '@rpath/%s' "${BASH_REMATCH[1]}"
    else
        printf '@rpath/%s' "$(basename "$p")"
    fi
}

# List every Homebrew absolute dependency of a file.
abs_deps() {
    otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^/opt/homebrew/' || true
}

macho_files() {
    find "$APP/Contents" -type f 2>/dev/null | while IFS= read -r f; do
        file "$f" 2>/dev/null | grep -q 'Mach-O' && printf '%s\n' "$f"
    done
}

# ---- Phase 1: recursively copy all non-Qt Homebrew dylib dependencies ----
copied=1
while [[ "$copied" -ne 0 ]]; do
    copied=0
    while IFS= read -r f; do
        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue
            name="$(basename "$dep")"
            case "$dep" in
                *.framework/*) continue ;;  # Qt frameworks already copied by macdeployqt
            esac
            if [[ ! -e "$FW/$name" ]]; then
                cp -f "$dep" "$FW/$name" 2>/dev/null && chmod u+w "$FW/$name" 2>/dev/null
                echo "COPY $name"
                copied=$((copied+1))
            fi
        done < <(abs_deps "$f")
    done < <(macho_files)
done

# ---- Phase 2: rewrite install names exactly once ----
while IFS= read -r f; do
    chmod u+w "$f" 2>/dev/null || true

    # own id (framework binary / dylib)
    id="$(otool -D "$f" 2>/dev/null | tail -n +2 | head -1)"
    case "$id" in
        /opt/homebrew/*)
            newid="$(to_rpath "$id")"
            install_name_tool -id "$newid" "$f" 2>/dev/null && echo "ID ${f#$APP/} -> $newid"
            ;;
    esac

    # dependencies
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        newdep="$(to_rpath "$dep")"
        install_name_tool -change "$dep" "$newdep" "$f" 2>/dev/null \
            && echo "LINK ${f#$APP/}: $(basename "$dep")"
    done < <(abs_deps "$f")

    # remove absolute Homebrew rpaths (leave @executable_path / @loader_path intact)
    while IFS= read -r rp; do
        [[ "$rp" == /opt/homebrew/* ]] || continue
        install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null \
            && echo "RPATH ${f#$APP/}: removed $rp"
    done < <(otool -l "$f" 2>/dev/null | awk '/LC_RPATH/{p=1} p&&/path /{print $2; p=0}')
done < <(macho_files)

# ---- Phase 3: sign bottom-up ----
while IFS= read -r f; do
    codesign --force --sign - "$f" 2>/dev/null
done < <(macho_files)
find "$FW" -maxdepth 1 -name '*.framework' -type d 2>/dev/null | while IFS= read -r d; do
    codesign --force --sign - "$d" 2>/dev/null
done
find "$APP/Contents/PlugIns" -type f 2>/dev/null | while IFS= read -r f; do
    codesign --force --sign - "$f" 2>/dev/null
done
codesign --force --sign - "$EXE"
codesign --force --sign - "$APP"

# ---- Phase 4: verification ----
echo "== verify =="
codesign --verify --deep --strict "$APP" && echo "SIGN_OK"
bad=0
while IFS= read -r f; do
    if abs_deps "$f" | grep -q .; then
        echo "REMAINS ${f#$APP/}:"
        abs_deps "$f" | head -3
        bad=$((bad+1))
    fi
done < <(macho_files)
echo "FILES_WITH_ABSOLUTE_DEPS=$bad"
