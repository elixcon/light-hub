# Host tests for the state machines in main/zb_coordinator.c and main/buttons.c,
# for the Matter lamps' command gate and endpoint discovery in
# main/matter_ctl.cpp and main/matter_commission.cpp,
# for the arithmetic in main/lighting.c, main/matter_bridge.cpp and
# main/settings.c, for the WLED interface's packets in main/wled.c, for the key
# format in main/auth.c, for the bootloader's ESP-IDF check in main/ota.c, for
# what a configuration backup carries and restores in
# main/config_backup.c, for the stack scan of a
# crash record in main/crash_log.c, for the leaf modules
# main/remote.c, main/sun.c, main/tz.c, main/ntc.c and main/zb_ota_file.c, and for the updater's boot
# decision and partition table plans in updater/main/.
#
#   make            build and run everything
#   make clean      remove the binaries and the extracted regions
#
# Needs nothing but a C compiler. No ESP-IDF, no hardware, a couple of seconds.
#
# The code under test is **extracted from the source at build time**, between
# the `host-test region` markers in the source file, and never copied into
# this directory. Two copies would drift, and the drift would be silent - the
# tests would go on passing against code that is no longer what ships. If a
# marker is moved or deleted the extraction produces nothing and the build stops
# with a message saying which region is missing.
#
# What is tested here is what is hard to produce on hardware and expensive to
# get wrong: the order commands reach a lamp in, and which lamp gets read when.
# Both fail on a real mesh as "that light is being temperamental", which no
# amount of staring at a serial log resolves - and the serial port reboots the
# board, so looking at all destroys the run being measured.
#
# To add a test for another region: mark it in the source, add it to REGIONS,
# and #include the generated .inc from your test file.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O1 -g -Wno-unused-function
SRC      = ../main/zb_coordinator.c
SRC_BTN  = ../main/buttons.c
SRC_LIGHT  = ../main/lighting.c
SRC_MATTER = ../main/matter_bridge.cpp
SRC_MCTL   = ../main/matter_ctl.cpp
SRC_MPAIR  = ../main/matter_commission.cpp
SRC_SET    = ../main/settings.c
SRC_AUTH   = ../main/auth.c
SRC_LDB    = ../main/light_db.c
SRC_LL     = ../main/local_lights.c
SRC_UPD    = ../updater/main/updater.c
SRC_CONV   = ../updater/main/convert.c

TESTS    = test_gate test_settle test_sweep test_zremote test_remote test_matter test_slug \
           test_unlock test_btnstep test_authkey test_switchpoint test_prewarm test_lightaddr test_cwww test_ramp test_updater test_convert \
           test_sun test_tz test_ntc test_alstate test_linkgap test_reach test_zbota test_fadein \
           test_mctl_gate test_mctl_disc test_clause test_parent test_crash test_ent test_stream test_wled \
           test_config_backup test_idfver
REGIONS  = gate-types gate-body settle-types settle-body sweep zremote btnstep levels matter slug \
           unlock authkey switchpoint prewarm fadein lightaddr cwww ramp pchip updater-decide convert-plan alstate linkgap reach zota-wake \
           mctl-gate mctl-disc clause parent crash-scan ent-frame stream wled-parse cfg-plan idf-ver
INCS     = $(REGIONS:%=%.inc)

.PHONY: all clean FORCE
all: $(TESTS)
	@for t in $(TESTS); do \
	    printf '%-14s ' "$$t"; ./$$t || exit 1; \
	done

# Everything between a region's two markers, the markers themselves included -
# they are comments, so they cost nothing and they show up in the extracted file
# when something needs debugging.
# Which file each region lives in.
FROM_gate-types   = $(SRC)
FROM_gate-body    = $(SRC)
FROM_settle-types = $(SRC)
FROM_settle-body  = $(SRC)
FROM_sweep        = $(SRC_BTN)
FROM_clause       = $(SRC_BTN)
FROM_zremote      = $(SRC_BTN)
FROM_btnstep      = $(SRC_BTN)
FROM_levels       = $(SRC_LIGHT)
FROM_matter       = $(SRC_MATTER)
FROM_slug         = $(SRC_SET)
FROM_unlock       = $(SRC_BTN)
FROM_authkey      = $(SRC_AUTH)
FROM_switchpoint  = $(SRC_LIGHT)
FROM_prewarm      = $(SRC_LIGHT)
FROM_fadein       = $(SRC_LIGHT)
FROM_lightaddr    = $(SRC_LDB)
FROM_cwww         = $(SRC_LL)
FROM_ramp         = $(SRC_LL)
FROM_pchip        = ../main/curve_db.c
FROM_linkgap      = $(SRC)
FROM_reach        = $(SRC)
FROM_parent       = $(SRC)
FROM_updater-decide = $(SRC_UPD)
FROM_convert-plan   = $(SRC_CONV)
FROM_alstate      = $(SRC_LIGHT)
FROM_zota-wake    = ../main/zb_ota.c
FROM_mctl-gate    = $(SRC_MCTL)
FROM_mctl-disc    = $(SRC_MPAIR)
FROM_crash-scan   = ../main/crash_log.c
FROM_ent-frame    = $(SRC)
FROM_stream       = $(SRC_LIGHT)
FROM_wled-parse   = ../main/wled.c
FROM_cfg-plan     = ../main/config_backup.c
FROM_idf-ver      = ../main/ota.c

# FORCE rather than a plain mtime dependency: make compares timestamps to the
# second, so an edit and a re-run inside the same second leaves the extracted
# copy stale and the tests pass against code that is no longer in the file -
# which is the one failure this whole arrangement exists to prevent, wearing a
# disguise. Extraction is a sed over one file, so doing it every time costs
# nothing, and neither does the rebuild it forces - the whole suite is a second.
.SECONDEXPANSION:
%.inc: $$(FROM_$$*) Makefile FORCE
	@sed -n '/>>> host-test region: $*  *<<</,/>>> end host-test region: $*  *<<</p' \
	    $(FROM_$*) > $@.tmp
	@test -s $@.tmp || { \
	    echo "test/Makefile: no '$*' region in $(FROM_$*)."; \
	    echo "  Either a marker was moved or deleted, or the region was renamed."; \
	    echo "  Look for '>>> host-test region: $* <<<'."; \
	    rm -f $@.tmp; exit 1; }
	@mv -f $@.tmp $@

# And the binaries for the same reason: the extracted copy having changed is no
# use if the compiler is not run over it, and the timestamps tie there too.
$(TESTS): FORCE

FORCE:

test_gate:   test_gate.c   gate-types.inc   gate-body.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_settle: test_settle.c gate-types.inc settle-types.inc settle-body.inc
	@$(CC) $(CFLAGS) -o $@ $<

# The region is plain C inside a C++ file, so it compiles as C here and as C++
# on the device - which is the point of keeping it free of namespaces.
test_matter: test_matter.c levels.inc matter.inc
	@$(CC) $(CFLAGS) -o $@ $<

# Plain C inside C++ files as well, like the `matter` region.
test_mctl_gate: test_mctl_gate.c mctl-gate.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_mctl_disc: test_mctl_disc.c mctl-disc.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_sweep:  test_sweep.c  sweep.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_clause: test_clause.c clause.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_btnstep: test_btnstep.c btnstep.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_zremote: test_zremote.c zremote.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_slug:   test_slug.c   slug.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_linkgap: test_linkgap.c linkgap.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_reach:  test_reach.c  reach.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_parent: test_parent.c parent.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_unlock: test_unlock.c unlock.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_authkey: test_authkey.c authkey.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_idfver: test_idfver.c idf-ver.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_switchpoint: test_switchpoint.c switchpoint.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_prewarm: test_prewarm.c prewarm.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_fadein: test_fadein.c fadein.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_updater: test_updater.c updater-decide.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_convert: test_convert.c convert-plan.inc
	@$(CC) $(CFLAGS) -o $@ $<

# The shape goes through zbc_pchip and, without points, zbc_al_mix: the real
# pchip region and the whole of sun.c, a leaf.
test_alstate: test_alstate.c alstate.inc pchip.inc ../main/sun.c ../main/sun.h ../main/light_math.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_alstate.c ../main/sun.c -lm

test_crash: test_crash.c crash-scan.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_ent:    test_ent.c    ent-frame.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_stream: test_stream.c stream.inc
	@$(CC) $(CFLAGS) -o $@ $< -lm

test_wled:   test_wled.c   wled-parse.inc
	@$(CC) $(CFLAGS) -o $@ $<

test_config_backup: test_config_backup.c cfg-plan.inc
	@$(CC) $(CFLAGS) -o $@ $<

# zb_ota_file.c is a leaf too: the Zigbee OTA file header and Query Next Image.
# zota-wake is a region of zb_ota.c, which is not.
test_zbota: test_zbota.c zota-wake.inc ../main/zb_ota_file.c ../main/zb_ota_file.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_zbota.c ../main/zb_ota_file.c

# sun.c is a leaf like remote.c, so it is compiled whole.
test_sun: test_sun.c ../main/sun.c ../main/sun.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_sun.c ../main/sun.c -lm

# tz.c too: the POSIX time zone rules the sunrise and sunset limits are read in.
test_tz: test_tz.c ../main/tz.c ../main/tz.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_tz.c ../main/tz.c

# ntc.c is a leaf as well: the NTC divider and the overheat decision.
test_ntc: test_ntc.c ../main/ntc.c ../main/ntc.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_ntc.c ../main/ntc.c -lm

# light_math.h is a leaf header, so these regions compile against the real one.
test_lightaddr: test_lightaddr.c lightaddr.inc ../main/light_math.h
	@$(CC) $(CFLAGS) -I../main -o $@ $<

test_cwww: test_cwww.c cwww.inc pchip.inc ../main/light_math.h
	@$(CC) $(CFLAGS) -I../main -o $@ $< -lm

test_ramp: test_ramp.c ramp.inc cwww.inc pchip.inc ../main/light_math.h
	@$(CC) $(CFLAGS) -I../main -o $@ $< -lm

# remote.c is a leaf - no ESP-IDF, nothing to extract - so it is compiled whole
# rather than lifted out of a bigger file. Same reason colorspace.c can be.
test_remote: test_remote.c ../main/remote.c ../main/remote.h
	@$(CC) $(CFLAGS) -I../main -o $@ test_remote.c ../main/remote.c

clean:
	@rm -rf $(TESTS) $(INCS) $(INCS:%=%.tmp) $(TESTS:%=%.dSYM)
