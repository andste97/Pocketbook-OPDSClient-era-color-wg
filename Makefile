# PocketBook SDK 6.8 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = OPDSClient.app
SRC = main.c logger.c network.c parser.c ui.c app_logic.c auth.c
OBJ = $(SRC:.c=.o)

# The SDK ships no OpenSSL static archive (its libssl.a is NSS), so build one
# from source. Must stay in the 1.0.2 series: the SDK's prebuilt libcurl.a uses
# APIs removed in OpenSSL 1.1+.
OPENSSL_VERSION = 1.0.2u
OPENSSL_SHA256 = ecd0c6ffb493dd06707d38b14bb4d8c2288bb7033735606569d8f90f89669d16
OPENSSL_URL = https://github.com/openssl/openssl/releases/download/OpenSSL_$(subst .,_,$(OPENSSL_VERSION))/openssl-$(OPENSSL_VERSION).tar.gz
OPENSSL_DIR = third_party/openssl-$(OPENSSL_VERSION)
OPENSSL_STAMP = $(OPENSSL_DIR)/.built

# Everything except libinkview and glibc is linked statically so the binary does
# not depend on the library versions that happen to ship with a given firmware
# (FW 6.10 no longer provides libjson-c.so.2, and its libcurl is older than the
# 7.62+ curl_url() API this app uses).
STATIC_LIBS = -Wl,-Bstatic \
              -lcurl -lxml2 -ljson-c -lcares -lssl -lcrypto -llzma -lz \
              -Wl,-Bdynamic

# libinkview.so and glibc are firmware-provided and cannot be static.
SYSTEM_LIBS = -linkview -lm -ldl -lpthread -lrt

# The self-built OpenSSL must be searched before the sysroot, whose libssl.a is
# NSS and would fail to resolve libcurl's SSL_* references.
LDFLAGS = -L$(OPENSSL_DIR) -L$(SYSROOT)/usr/lib \
          -Wl,--gc-sections \
          -Wl,--exclude-libs,ALL \
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

$(APP_NAME): $(OBJ) $(OPENSSL_STAMP)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

$(OPENSSL_STAMP):
	mkdir -p third_party
	cd third_party && rm -rf openssl-$(OPENSSL_VERSION) && \
		wget -q -O openssl-$(OPENSSL_VERSION).tar.gz "$(OPENSSL_URL)" && \
		echo "$(OPENSSL_SHA256)  openssl-$(OPENSSL_VERSION).tar.gz" | sha256sum -c - && \
		tar xzf openssl-$(OPENSSL_VERSION).tar.gz && \
		rm openssl-$(OPENSSL_VERSION).tar.gz
	cd $(OPENSSL_DIR) && ./Configure linux-armv4 no-shared no-dso threads \
		--cross-compile-prefix=$(SDK_PATH)/bin/arm-obreey-linux-gnueabi-
	$(MAKE) -C $(OPENSSL_DIR) build_libs
	touch $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME) tests/test_runner

distclean: clean
	rm -rf third_party

.PHONY: all test clean distclean
