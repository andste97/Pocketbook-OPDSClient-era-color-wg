# PocketBook SDK 6.8 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = OPDSClient.app
SRC = main.c logger.c network.c parser.c ui.c app_logic.c auth.c
OBJ = $(SRC:.c=.o)

# The SDK's OpenSSL is 1.0.2o and it ships no OpenSSL static archive at all
# (its libssl.a is NSS), so build a current one from source. Its prebuilt
# libcurl.a is pinned to the 1.0.2 API, so curl is rebuilt against it too.
THIRD_PARTY = third_party
DEPS_PREFIX = $(CURDIR)/$(THIRD_PARTY)/prefix
# Stamps live outside the source trees so CI only needs to cache prefix/ and
# stamps/, not the multi-hundred-megabyte unpacked build directories.
STAMP_DIR = $(THIRD_PARTY)/stamps

OPENSSL_VERSION = 3.5.7
OPENSSL_SHA256 = a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
OPENSSL_URL = https://github.com/openssl/openssl/releases/download/openssl-$(OPENSSL_VERSION)/openssl-$(OPENSSL_VERSION).tar.gz
OPENSSL_DIR = $(THIRD_PARTY)/openssl-$(OPENSSL_VERSION)
OPENSSL_STAMP = $(STAMP_DIR)/openssl-$(OPENSSL_VERSION)

CURL_VERSION = 8.21.0
CURL_SHA256 = d9b327997999045a24cda50f3983e69e51c516bd8be6ef9842fc7f99135e33bb
CURL_URL = https://github.com/curl/curl/releases/download/curl-$(subst .,_,$(CURL_VERSION))/curl-$(CURL_VERSION).tar.gz
CURL_DIR = $(THIRD_PARTY)/curl-$(CURL_VERSION)
CURL_STAMP = $(STAMP_DIR)/curl-$(CURL_VERSION)

# The SDK's c-ares is 1.12 and lacks ares_getaddrinfo, which curl 8.x requires.
# Keeping a resolver library also avoids curl's threaded resolver, which would
# dlopen the firmware's glibc NSS modules at runtime.
CARES_VERSION = 1.34.8
CARES_SHA256 = c222b6d681096f9444d2c4863d2c1174019e27cacca0a4a5c114d36dd7d7bf78
CARES_URL = https://github.com/c-ares/c-ares/releases/download/v$(CARES_VERSION)/c-ares-$(CARES_VERSION).tar.gz
CARES_DIR = $(THIRD_PARTY)/c-ares-$(CARES_VERSION)
CARES_STAMP = $(STAMP_DIR)/c-ares-$(CARES_VERSION)

CROSS_PREFIX = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-
DEVICE_CA_BUNDLE = /ebrmain/share/ssl/certs/ca-certificates.crt

# autotools invokes the cross tools unprefixed, so they must be reachable for
# every recipe, not just ./configure. Appended so SDK wrappers don't shadow the
# host's build tools.
export PATH := $(PATH):$(SDK_PATH)/bin

# Everything except libinkview and glibc is linked statically so the binary does
# not depend on the library versions that happen to ship with a given firmware
# (FW 6.10 no longer provides libjson-c.so.2, and its libcurl is older than the
# 7.62+ curl_url() API this app uses).
STATIC_LIBS = -Wl,-Bstatic \
              -lcurl -lxml2 -ljson-c -lcares -lssl -lcrypto -llzma -lz \
              -Wl,-Bdynamic

# libinkview.so and glibc are firmware-provided and cannot be static.
SYSTEM_LIBS = -linkview -lm -ldl -lpthread -lrt

# The self-built curl and OpenSSL must be searched before the sysroot, which
# still holds the SDK's libcurl 7.82 and an NSS libssl.a.
LDFLAGS = -L$(DEPS_PREFIX)/lib -L$(SYSROOT)/usr/lib \
          -Wl,--gc-sections \
          -Wl,--exclude-libs,ALL \
          $(STATIC_LIBS) $(SYSTEM_LIBS)

CFLAGS = -Wall -O2 -g -fno-omit-frame-pointer \
         -ffunction-sections -fdata-sections \
         -I$(DEPS_PREFIX)/include \
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

$(APP_NAME): $(OBJ) $(CURL_STAMP)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

$(OBJ): $(CURL_STAMP)

# $(1) url, $(2) basename, $(3) sha256
define fetch
	mkdir -p $(STAMP_DIR)
	cd $(THIRD_PARTY) && rm -rf $(2) && \
		wget -q -O $(2).tar.gz "$(1)" && \
		echo "$(3)  $(2).tar.gz" | sha256sum -c - && \
		tar xzf $(2).tar.gz && rm $(2).tar.gz
endef

$(OPENSSL_STAMP):
	$(call fetch,$(OPENSSL_URL),openssl-$(OPENSSL_VERSION),$(OPENSSL_SHA256))
	cd $(OPENSSL_DIR) && ./Configure linux-armv4 no-shared no-dso no-tests no-docs no-apps \
		threads --cross-compile-prefix=$(CROSS_PREFIX) \
		--prefix=$(DEPS_PREFIX) --openssldir=/etc/ssl
	$(MAKE) -C $(OPENSSL_DIR) build_libs
	$(MAKE) -C $(OPENSSL_DIR) install_dev
	touch $@

$(CARES_STAMP):
	$(call fetch,$(CARES_URL),c-ares-$(CARES_VERSION),$(CARES_SHA256))
	cd $(CARES_DIR) && ./configure \
		--host=arm-obreey-linux-gnueabi --build=x86_64-pc-linux-gnu \
		--prefix=$(DEPS_PREFIX) --disable-shared --enable-static --disable-tests
	$(MAKE) -C $(CARES_DIR)
	$(MAKE) -C $(CARES_DIR) install
	touch $@

# The SDK's pkg-config wrapper rewrites every prefix into the ARM sysroot, which
# turns our paths into <sysroot>/project/third_party/... and makes curl fall
# back to the SDK's OpenSSL 1.0.2. Neutralise it for this configure run.
$(CURL_STAMP): $(OPENSSL_STAMP) $(CARES_STAMP)
	$(call fetch,$(CURL_URL),curl-$(CURL_VERSION),$(CURL_SHA256))
	cd $(CURL_DIR) && PKG_CONFIG_SYSROOT_DIR=/ \
		./configure --host=arm-obreey-linux-gnueabi --build=x86_64-pc-linux-gnu \
		--prefix=$(DEPS_PREFIX) --disable-shared --enable-static \
		--with-openssl=$(DEPS_PREFIX) \
		--enable-ares=$(DEPS_PREFIX) --with-ca-bundle=$(DEVICE_CA_BUNDLE) \
		--without-libpsl --without-libidn2 --without-nghttp2 --without-brotli --without-zstd \
		--disable-manual --disable-docs --disable-ldap --disable-ldaps --disable-ftp \
		--disable-file --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
		--disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt
	$(MAKE) -C $(CURL_DIR)
	$(MAKE) -C $(CURL_DIR) install
	touch $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME) tests/test_runner

distclean: clean
	rm -rf $(THIRD_PARTY)

.PHONY: all test clean distclean
