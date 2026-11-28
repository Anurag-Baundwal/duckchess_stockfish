#!/bin/sh

# Downloads wakwak's network (github.com/Silverrzz/wakwak-nets), as Stockfish's
# scripts/net.sh does for its networks. Run from src/ by "make net".

filename="shotis.nnue"
url="https://github.com/Silverrzz/wakwak-nets/releases/download/shotis/shotis.nnue"
sha="5a080efa1aa5381cc8efc7d2ca89f1f5ea20d3397ead83c3c7c12ef3b28e29cd"

# download commands with a 5min time-out to ensure things fail if the server stalls
wget_or_curl=$( (command -v curl >/dev/null 2>&1 && echo "curl -skL --max-time 300") ||
  (command -v wget >/dev/null 2>&1 && echo "wget -qO- --timeout=300 --tries=1"))

sha256sum=$( (command -v sha256sum >/dev/null 2>&1 && echo "sha256sum") ||
  (command -v shasum >/dev/null 2>&1 && echo "shasum -a 256"))

if [ -z "$sha256sum" ]; then
  >&2 echo "sha256sum not found, the network will be assumed valid."
fi

validate_network() {
  # If no sha256sum command is available, assume the file is always valid.
  if [ -n "$sha256sum" ] && [ -f "$1" ]; then
    if [ "$sha" != "$($sha256sum "$1" | cut -c 1-64)" ]; then
      rm -f "$1"
      return 1
    fi
  fi
}

if [ -f "$filename" ]; then
  if validate_network "$filename"; then
    echo "Existing $filename validated, skipping download"
    exit 0
  else
    echo "Removing invalid network: $filename"
  fi
fi

if [ -z "$wget_or_curl" ]; then
  >&2 printf "%s\n" "Neither wget or curl is installed." \
    "Install one of these tools to download the network automatically."
  exit 1
fi

for attempt in 1 2 3; do
  echo "Downloading from $url ..."
  if $wget_or_curl "$url" >"$filename" && validate_network "$filename"; then
    echo "Successfully validated $filename"
    exit 0
  fi
  rm -f "$filename"
  echo "Download attempt $attempt failed"
done

>&2 echo "Failed to download $filename"
exit 1
