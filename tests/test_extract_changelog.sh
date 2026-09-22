#!/usr/bin/env bash
#
# Covers tools/extract_changelog.sh, which feeds the release workflow's notes.
#
# The bug this starts from: the extraction compared a whole heading line against
# "## v<version>", so the dated headings the file actually uses never matched and
# every release was published with empty notes - quietly, because an empty
# section looked the same as a version with nothing to say.

set -uo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
extract="$script_dir/../tools/extract_changelog.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

failures=0
checks=0

check() {
    local what=$1 expected=$2 actual=$3
    checks=$((checks + 1))
    if [ "$expected" = "$actual" ]; then
        echo "ok   - $what"
    else
        echo "FAIL - $what"
        echo "       expected: $(printf '%q' "$expected")"
        echo "       actual:   $(printf '%q' "$actual")"
        failures=$((failures + 1))
    fi
}

check_status() {
    local what=$1 expected=$2; shift 2
    "$@" >/dev/null 2>&1
    check "$what" "$expected" "$?"
}

cat > "$work/CHANGELOG.md" <<'CHANGELOG'
# Changelog

## v0.3.0 - 11-05-2026

Third.

### Section
- bullet

## v0.2.0

Second.

## v0.1.0-rc1 - 09-01-2026

Prerelease.

## v0.1.0 - 09-22-2026

First.
CHANGELOG

# The regression: a dated heading has to be found. This is what returned
# nothing before the fix.
check "dated heading is matched" \
    "First." "$("$extract" v0.1.0 "$work/CHANGELOG.md")"

check "bare heading is matched" \
    "Second." "$("$extract" v0.2.0 "$work/CHANGELOG.md")"

check "version may be given without the v prefix" \
    "Second." "$("$extract" 0.2.0 "$work/CHANGELOG.md")"

# A section ends at the next "## ", and nowhere earlier - "### " subheadings
# are part of the notes.
check "section stops at the next ## heading, keeping ### subheadings" \
    "$(printf 'Third.\n\n### Section\n- bullet')" \
    "$("$extract" v0.3.0 "$work/CHANGELOG.md")"

# A prefix match would hand v0.1.0's release the rc1 notes, or vice versa.
check "a prerelease suffix is not a match for the release" \
    "Prerelease." "$("$extract" v0.1.0-rc1 "$work/CHANGELOG.md")"

# Failing loudly is the other half of the fix: an unpublishable extraction must
# stop the release rather than draft empty notes.
check_status "a missing version fails" 1 "$extract" v9.9.9 "$work/CHANGELOG.md"

cat > "$work/empty.md" <<'CHANGELOG'
# Changelog

## v1.0.0 - 01-01-2027

## v0.9.0

Notes.
CHANGELOG

check_status "a version with an empty section fails" 1 "$extract" v1.0.0 "$work/empty.md"
check_status "a missing changelog file fails" 2 "$extract" v0.1.0 "$work/nope.md"

# The real file has to work, since that is what the release actually reads.
check_status "the repository CHANGELOG.md yields notes for its latest version" 0 \
    "$extract" "$(awk '/^##[[:space:]]/ { sub(/^##[[:space:]]+/, ""); print $1; exit }' \
        "$script_dir/../CHANGELOG.md")" "$script_dir/../CHANGELOG.md"

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures of $checks checks failed"
    exit 1
fi
echo "all $checks checks passed"
