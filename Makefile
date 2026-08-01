# PocketBook SDK 6.8 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = OPDSClient.app
SRC = main.c logger.c network.c parser.c ui.c app_logic.c auth.c
OBJ = $(SRC:.c=.o)

# Everything except libinkview and glibc is linked statically so the binary does
# not depend on the library versions that happen to ship with a given firmware
# (FW 6.10 no longer provides libjson-c.so.2, and its libcurl is older than the
# 7.62+ curl_url() API this app uses).
STATIC_LIBS = -Wl,-Bstatic \
              -lcurl -lxml2 -ljson-c -lcares -llzma -lz \
              -Wl,-Bdynamic

# libinkview.so and glibc are firmware-provided and cannot be static.
# libssl/libcrypto have no OpenSSL static archive in the SDK (libssl.a is NSS),
# so they stay dynamic; `make bundle` ships copies next to the app and the
# RPATH below makes the loader prefer them over the firmware's.
SYSTEM_LIBS = -linkview -lssl -lcrypto -lm -ldl -lpthread -lrt

BUNDLE_DIR = OPDSClient/lib
BUNDLED_SONAMES = libssl.so.1.0.0 libcrypto.so.1.0.0

LDFLAGS = -L$(SYSROOT)/usr/lib \
          -Wl,--gc-sections \
          -Wl,--exclude-libs,ALL \
          -Wl,-rpath,'$$ORIGIN/$(BUNDLE_DIR)' \
          $(STATIC_LIBS) $(SYSTEM_LIBS)

CFLAGS = -Wall -O2 -g -fno-omit-frame-pointer \
         -ffunction-sections -fdata-sections \
         -I$(SYSROOT)/usr/include \
         -I$(SYSROOT)/usr/include/libxml2 \
         -I$(SYSROOT)/usr/include/freetype2

all: $(APP_NAME)

TEST_IMAGE = pocketbook-opds-client-tests
TEST_SRC = tests/test_runner.c logger.c parser.c app_logic.c auth.c network.c

test:
	docker build -q -t $(TEST_IMAGE) -f tests/Dockerfile .
	docker run --rm -v "$(CURDIR):/project" -w /project $(TEST_IMAGE) \
		sh -lc 'gcc -std=c11 -Wall -Wextra -Werror -DEMULATOR -DUNIT_TEST -I. \
		$$(pkg-config --cflags libxml-2.0 libcurl json-c freetype2) \
		$(TEST_SRC) -o tests/test_runner \
		$$(pkg-config --libs libxml-2.0 libcurl json-c freetype2) -lm -ldl -lpthread \
		&& ./tests/test_runner'

$(APP_NAME): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# Produces the directory layout to copy onto /mnt/ext1/applications so the app
# uses its own OpenSSL instead of whatever the firmware ships.
bundle: $(APP_NAME)
	mkdir -p dist/$(BUNDLE_DIR)
	cp $(APP_NAME) dist/
	for lib in $(BUNDLED_SONAMES); do cp $(SYSROOT)/usr/lib/$$lib dist/$(BUNDLE_DIR)/; done

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME) tests/test_runner
	rm -rf dist
