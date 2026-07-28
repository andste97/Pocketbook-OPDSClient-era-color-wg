# PocketBook SDK 6.8 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = OPDSClient.app
SRC = main.c logger.c network.c parser.c ui.c app_logic.c auth.c
OBJ = $(SRC:.c=.o)

# LDFLAGS: Changed -lft2 to -lfreetype
# Added explicit path to the sysroot library folder where libfreetype.so lives
LDFLAGS = -L$(SYSROOT)/usr/lib \
          -linkview -lcurl -lxml2 -ljson-c -lfreetype -lm -ldl

CFLAGS = -Wall -O2 -g -fno-omit-frame-pointer \
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

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME) tests/test_runner
