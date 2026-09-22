#!/usr/bin/env bash
#
# Print the CHANGELOG.md section for one version, for use as release notes.
#
#   extract_changelog.sh <version> [changelog path]
#
# <version> is matched against the first whitespace-delimited token of a "## "
# heading, with or without a leading "v", so both of these are found by
# "v0.1.0" and by "0.1.0":
#
#   ## v0.1.0
#   ## v0.1.0 - 09-22-2026
#
# Matching the token rather than the whole line is the point: the dated form is
# what the file actually uses, and a whole-line compare silently produces empty
# release notes instead of failing. "### " subheadings are body, not section
# boundaries, so only a "## " heading ends the section.
#
# The blank line a heading is followed by is trimmed, so the notes start at the
# first line of prose.
#
# Exits non-zero if the version has no section, or if the section is empty.

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 <version> [changelog path]" >&2
    exit 2
fi

version="v${1#v}"
changelog="${2:-CHANGELOG.md}"

if [ ! -f "$changelog" ]; then
    echo "$0: no such file: $changelog" >&2
    exit 2
fi

notes=$(awk -v ver="$version" '
    /^##[[:space:]]/ {
        if (found) exit
        heading = $0
        sub(/^##[[:space:]]+/, "", heading)
        split(heading, token, /[[:space:]]/)
        if (token[1] == ver) { found = 1 }
        next
    }
    found { print }
' "$changelog" | sed -e '/./,$!d')

if ! printf '%s' "$notes" | grep -q '[^[:space:]]'; then
    echo "$0: no $version section in $changelog" >&2
    exit 1
fi

printf '%s\n' "$notes"
