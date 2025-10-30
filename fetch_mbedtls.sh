#!/usr/bin/env bash
set -e

# Fetch mbedTLS into vendor/mbedtls
# This script clones the official mbed TLS repository at a known-good tag.
# After fetching, you will need to update build.sh to compile needed sources
# (or run the provided helper to generate a small static subset).

VENDOR_DIR="vendor/mbedtls"
TAG="v2.28.0"

if [ -d "$VENDOR_DIR" ]; then
  echo "mbedTLS already fetched in $VENDOR_DIR"
  exit 0
fi

echo "Cloning mbed TLS ${TAG} into ${VENDOR_DIR}..."
mkdir -p vendor
if command -v git >/dev/null 2>&1; then
  git clone --depth 1 --branch ${TAG} https://github.com/ARMmbed/mbedtls.git ${VENDOR_DIR}
  echo "Cloned mbed TLS. See ${VENDOR_DIR} for sources."
else
  echo "git not available. Please install git or download mbed TLS ${TAG} manually and extract to ${VENDOR_DIR}."
  exit 1
fi

echo "Fetch complete. Next steps:
  1) Edit build.sh to add the required mbedTLS source files to compile/link (or create a small amalgam).
  2) Implement a thin TLS glue layer in tls_mbedtls.c that uses the kernel tcp I/O functions.
  3) Rebuild with build.sh.
"
