CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pthread

# Which ALSA format native DSD is sent as. This is a property of the DAC,
# not a preference: `cat /proc/asound/cardN/stream0` shows the one your
# device advertises (look for "Format: SPECIAL DSD_U32_BE" or similar).
# Getting it wrong is not a graceful failure — the byte order is simply
# reversed and you hear noise. Default matches the most common XMOS
# firmware; override when your DAC says otherwise:
#
#     make DSD_FORMAT=DSD_U32_BE
#
DSD_FORMAT ?= DSD_U32_LE
CFLAGS += -DHALO_DSD_ALSA_FORMAT=SND_PCM_FORMAT_$(DSD_FORMAT)
LDFLAGS ?= -lasound -pthread

SRC = src/main.c src/alsa_output.c
OBJ = $(SRC:.c=.o)
BIN = halo-daemon

.PHONY: all clean check check-linux check-race check-pause

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

# Offline self-test: build against the bundled ALSA stub and drive the
# daemon through every protocol message, asserting replies and the runtime
# files it publishes. No ALSA, no DAC and no root required — the point is to
# catch regressions here in seconds rather than on the Pi.
#
# It cannot test what only real hardware can: hw_params negotiation, whether
# the DAC accepts native DSD, DSD bit order, or how it sounds.
STUB_DIR = tools/alsa-stub
CHECK_RUNTIME = /tmp/halo-selftest
CHECK_PORT = 5601

# Drives FLUSH and hard-cut FORMAT against a live audio feed, with the stub's
# writes made to block so they actually overlap the control path. Asserts the
# daemon never sees -EBADFD — the error the network thread's snd_pcm_drop()
# used to hand the writer thread mid-write, killing playback on every seek
# and every stop. Verified to fail (hundreds of hits) with the alsa_mtx
# locking removed, so it is a real regression test and not a tautology.
# Overfills a paused stream, then checks the daemon still answers control
# messages. Pre-fix this hangs outright: the reader blocked placing audio into
# a ring that a stopped writer could never drain, and that same thread is what
# delivers RESUME — so every pause at DSD rates wedged the daemon until it was
# restarted. Verified to fail with the overflow path removed.
check-pause:
	@echo "==> Paused-and-overfull control-path check"
	@rm -rf /tmp/halo-pause-rt && mkdir -p /tmp/halo-pause-rt
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -I src -I $(STUB_DIR) \
		-DHALO_RUNTIME_DIR='"/tmp/halo-pause-rt"' \
		src/main.c src/alsa_output.c $(STUB_DIR)/alsa_stub.c -o /tmp/halo-pause-daemon
	@/tmp/halo-pause-daemon stub:0,0 5651 > /tmp/halo-pause.log 2>&1 & \
		echo $$! > /tmp/halo-pause.pid
	@sleep 1.5
	@python3 tools/pause_stall_test.py 5651; rc=$$?; \
		kill `cat /tmp/halo-pause.pid` 2>/dev/null; \
		exit $$rc

check-race:
	@echo "==> FLUSH/FORMAT race stress"
	@rm -rf /tmp/halo-race-rt && mkdir -p /tmp/halo-race-rt
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -I src -I $(STUB_DIR) \
		-DHALO_RUNTIME_DIR='"/tmp/halo-race-rt"' \
		src/main.c src/alsa_output.c $(STUB_DIR)/alsa_stub.c -o /tmp/halo-race-daemon
	@HALO_STUB_WRITE_DELAY_US=4000 /tmp/halo-race-daemon stub:0,0 5641 > /tmp/halo-race.log 2>&1 & \
		echo $$! > /tmp/halo-race.pid
	@sleep 1.5
	@python3 tools/race_stress.py 5641 6; rc=$$?; \
		kill `cat /tmp/halo-race.pid` 2>/dev/null; \
		hits=`grep -c 'bad state' /tmp/halo-race.log || true`; \
		if [ "$$hits" != "0" ]; then \
			echo "  FAIL  $$hits ALSA bad-state hits — the device is being touched from two threads"; \
			exit 1; \
		fi; \
		if [ $$rc -ne 0 ]; then exit $$rc; fi; \
		echo "  PASS  no ALSA bad-state hits under FLUSH/FORMAT storm"

check:
	@$(MAKE) --no-print-directory check-race
	@$(MAKE) --no-print-directory check-pause
	@echo "==> Running sample-packing tests"
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread -I src -I $(STUB_DIR) \
		tools/packing_test.c $(STUB_DIR)/alsa_stub.c -o /tmp/halo-packing-test
	@/tmp/halo-packing-test
	@echo "==> Building against the ALSA stub"
	@rm -rf $(CHECK_RUNTIME) && mkdir -p $(CHECK_RUNTIME)
	@$(CC) -std=c11 -Wall -Wextra -Werror -pthread \
		-I src -I $(STUB_DIR) \
		-DHALO_RUNTIME_DIR='"$(CHECK_RUNTIME)"' \
		src/main.c src/alsa_output.c $(STUB_DIR)/alsa_stub.c \
		-o /tmp/halo-daemon-selftest
	@echo "==> Running protocol self-test"
	@/tmp/halo-daemon-selftest stub:0,0 $(CHECK_PORT) > /tmp/halo-selftest.log 2>&1 & \
		echo $$! > /tmp/halo-selftest.pid
	@sleep 1
	@python3 tools/protocol_selftest.py $(CHECK_PORT) $(CHECK_RUNTIME); \
		status=$$?; \
		kill `cat /tmp/halo-selftest.pid` 2>/dev/null; \
		rm -f /tmp/halo-selftest.pid; \
		if [ $$status -ne 0 ]; then echo; echo "daemon log:"; cat /tmp/halo-selftest.log; fi; \
		exit $$status

# Stricter sibling of `check`: same self-test, but compiled against the real
# libasound headers on the Pi's architecture inside a container, where
# snd_pcm_open genuinely fails and so exercises the FORMAT_REJECTED path the
# stub cannot reach. Needs docker (colima works).
check-linux:
	@tools/check-linux.sh
