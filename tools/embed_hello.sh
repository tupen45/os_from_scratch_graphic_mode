#!/usr/bin/env bash
set -e
if [ ! -f out/hello.so ]; then
  echo "Run 'make userprog' first to produce out/hello.so"
  exit 1
fi
xxd -i out/hello.so > userprog_hello_inc.c
cat > userprog_blob_inc.c <<'EOF'
#include "userprog_hello.h"
#include "userprog_hello_inc.c"
unsigned char out_hello_so[] = {
EOF
cat userprog_hello_inc.c | sed -n '1,200p' >> userprog_blob_inc.c || true
cat >> userprog_blob_inc.c <<'EOF'
};

size_t out_hello_so_len = sizeof(out_hello_so);

const unsigned char *get_hello_ptr(void) {
    return out_hello_so;
}

size_t get_hello_len(void) {
    return out_hello_so_len;
}
EOF
# Move into place
mv userprog_blob_inc.c userprog_blob.c
rm -f userprog_hello_inc.c
# Emit header
cat > userprog_hello.h << 'EOF'
#ifndef USERPROG_HELLO_H
#define USERPROG_HELLO_H

#include <stddef.h>

extern const unsigned char *get_hello_ptr(void);
extern size_t get_hello_len(void);

#endif
EOF
echo "Embedded out/hello.so -> userprog_blob.c and wrote userprog_hello.h"
