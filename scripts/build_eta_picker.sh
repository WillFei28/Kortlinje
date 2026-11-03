#!/usr/bin/env bash
# Local test page only: refresh CSVs under local/eta-picker-test/data.
# Device catalogs are downloaded at runtime from opendata.mtr.com.hk → LittleFS
# (not embedded). Optional: mirror into webui/catalog for offline reference.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo "Device: catalogs fetch at runtime (EtaPickerExtension)."
echo "Local test CSVs: $ROOT/local/eta-picker-test/data"
ls -la "$ROOT/local/eta-picker-test/data"
