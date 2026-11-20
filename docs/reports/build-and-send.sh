#!/usr/bin/env bash
# Compiles a report in this directory to PDF and copies it to big-cow's
# ~/projects/Inbox, or to this machine's ~/projects/Inbox if big-cow is down.
#
#   docs/reports/build-and-send.sh 2026-11-15-binary-format-report

set -euo pipefail

report_directory="$(cd "$(dirname "$0")" && pwd)"
report_name="${1:?usage: build-and-send.sh <report name without .tex>}"
build_directory="$report_directory/build"
destination_host="big-cow"
destination_directory="projects/Inbox"

mkdir -p "$build_directory"
latexmk -xelatex -interaction=nonstopmode -halt-on-error \
    -outdir="$build_directory" "$report_directory/$report_name.tex" >/dev/null

connection_timeout_seconds=10

if ssh -o BatchMode=yes -o ConnectTimeout="$connection_timeout_seconds" \
    "$destination_host" "mkdir -p ~/$destination_directory" 2>/dev/null; then
    scp -q "$build_directory/$report_name.pdf" "$destination_host:$destination_directory/"
    echo "sent $report_name.pdf to $destination_host:~/$destination_directory/"
else
    mkdir -p "$HOME/$destination_directory"
    cp "$build_directory/$report_name.pdf" "$HOME/$destination_directory/"
    echo "$destination_host unreachable; copied $report_name.pdf to ~/$destination_directory/ on $(hostname)"
fi
