# SPDX-License-Identifier: GPL-2.0
#
# Builds SMSC95xxDriver.dext and the installer app that loads it, without an Xcode
# project. Every flag here was validated by hand before this file existed; the
# comments record why the non-obvious ones are required.

DEVELOPER_DIR := /Applications/Xcode.app/Contents/Developer
export DEVELOPER_DIR

DEXT_NAME    := SMSC95xxDriver
DEXT_ID      := com.github.stewartallen.smsc95xx.driver
APP_NAME     := SMSC95xxInstaller
APP_ID       := com.github.stewartallen.smsc95xx

# Signing. Empty SIGN_ID means ad-hoc, which needs amfi_get_out_of_my_way=0x1 because the
# restricted entitlements are then unauthorised. That boot-arg disables entitlement
# validation machine-wide and is NOT the supported path -- set SIGN_ID to a real identity
# and pass both profiles, which authorises the entitlements properly and loads with AMFI
# enforcing. Configure that once in .env (see .env.example), or override per invocation:
#   make install SIGN_ID="Apple Development: you@example.com (XXXXXXXXXX)" \
#                APP_PROFILE=~/app.provisionprofile DEXT_PROFILE=~/dext.provisionprofile
# A profile only authorises an entitlement whose value it grants EXACTLY: the "*" in the
# profile's transport.usb grant is a literal to reproduce, not a wildcard to narrow. See
# SMSC95xxDriver/SMSC95xxDriver.entitlements.in and reference/signing-entitlement-match.txt.
# Local signing configuration lives in .env, which is gitignored: the signing identity
# names a person, and the profile paths point at files that must never be committed.
# Copy .env.example to .env and fill it in. Command-line variables still win over .env,
# and a missing .env is fine -- you get an ad-hoc build.
-include .env

SIGN_ID      ?=
APP_PROFILE   ?=
DEXT_PROFILE  ?=

# .env is parsed by make, not by a shell, so values are taken literally: quotes become
# part of the value and neither ~ nor $VAR is expanded. Both are easy to write out of
# habit and both fail confusingly -- a quoted SIGN_ID reaches codesign with the quotes
# attached, and a quoted "~/x.provisionprofile" is passed through as a literal path that
# does not exist. Undo both here so either style works.
strip_quotes  = $(subst ",,$(subst ',,$(1)))
expand_home   = $(patsubst ~/%,$(HOME)/%,$(1))
SIGN_ID      := $(call strip_quotes,$(SIGN_ID))
APP_PROFILE  := $(call expand_home,$(call strip_quotes,$(APP_PROFILE)))
DEXT_PROFILE := $(call expand_home,$(call strip_quotes,$(DEXT_PROFILE)))

CODESIGN_ID  := $(if $(SIGN_ID),$(SIGN_ID),-)

# Catch a path typo before it turns into a silently ad-hoc build: a missing profile means
# the restricted entitlements go unauthorised, which signs and verifies cleanly and only
# fails at launch.
$(foreach p,$(APP_PROFILE) $(DEXT_PROFILE),\
    $(if $(wildcard $(p)),,$(error profile not found: $(p) -- check .env)))

# No Apple Team ID is checked in: the entitlements files are templates carrying
# __TEAM_ID__, and the real value is read out of the provisioning profile at build
# time. Reading it from the profile rather than hardcoding it means it cannot drift
# from the profile it has to match, and it lets anyone build this with their own
# account. Override TEAM_ID only if you are signing without a profile.
#
# A .provisionprofile is CMS-signed, so the plist has to be unwrapped before the
# Entitlements dict can be read.
# loads(), not load(): a pipe is not seekable and plistlib.load() insists on seeking.
team_id_of = $(shell security cms -D -i "$(1)" 2>/dev/null | /usr/bin/python3 -c \
    'import plistlib,sys; print(plistlib.loads(sys.stdin.buffer.read())["Entitlements"]["com.apple.developer.team-identifier"])' 2>/dev/null)
TEAM_ID      ?= $(if $(DEXT_PROFILE),$(call team_id_of,$(DEXT_PROFILE)),$(if $(APP_PROFILE),$(call team_id_of,$(APP_PROFILE)),))
TEAM_ID      := $(strip $(TEAM_ID))

ifeq ($(TEAM_ID),)
ifeq ($(strip $(SIGN_ID)),)
# An ad-hoc build is not profile-backed, so the two keys that tie the binary to a profile
# are never validated. Substituting an obviously fake Team ID keeps a bare `make` working
# with no configuration at all, while the error below still refuses a *signed* build that
# has no real one.
TEAM_ID      := ADHOCNOTEAM
else
# Fail early and loudly. Signing for real with an unsubstituted or wrong Team ID produces
# a bundle that signs and verifies cleanly, then dies at launch under AMFI -- the most
# expensive failure mode to diagnose, so do not let the build reach it.
$(error SIGN_ID is set but no TEAM_ID could be read from a profile. Set DEXT_PROFILE and \
APP_PROFILE in .env, or set TEAM_ID explicitly)
endif
endif

BUILD        := build
# The bundle and its executable must be named after the bundle IDENTIFIER, not the
# project name. Named SMSC95xxDriver.dext instead, OSSystemExtensionRequest fails
# with OSSystemExtensionErrorDomain code 4, "Unable to find any matched extension
# with identifier ...", even though the Info.plist identifier is correct. Apple's
# own dexts in /System/Library/DriverExtensions follow the same convention.
DEXT_BUNDLE  := $(BUILD)/$(DEXT_ID).dext
APP_BUNDLE   := $(BUILD)/$(APP_NAME).app
GEN          := $(BUILD)/gen

# Generated from the .in templates with the real Team ID substituted in, so they land
# in build/ and are never checked in. Declared after GEN because := expands now.
DEXT_ENT     := $(GEN)/$(DEXT_NAME).entitlements
APP_ENT      := $(GEN)/Installer.entitlements

# $(shell) is evaluated at parse time and does not inherit the `export` above,
# so DEVELOPER_DIR has to be set inline here or the SDK path comes back empty.
DK_SDK       := $(shell DEVELOPER_DIR=$(DEVELOPER_DIR) xcrun --sdk driverkit --show-sdk-path)

ifeq ($(strip $(DK_SDK)),)
$(error Could not locate the DriverKit SDK. Is Xcode installed at $(DEVELOPER_DIR)?)
endif

# arm64e, NOT arm64. A plain arm64 dext builds, links, signs, stages and activates
# perfectly, then fails at launch with kernelmanagerd reporting
# NSPOSIXErrorDomain Code=8 "Exec format error" -- with nothing wrong in the
# IOKit personality. Apple's own dexts in /System/Library/DriverExtensions are
# arm64e; DriverKit on Apple Silicon requires pointer authentication.
DK_TARGET    := arm64e-apple-driverkit24.0
COMMON       := ../common

# iig rejects -arch and -target outright, and needs -D__IIG=1 or every SDK .iig
# header fails to parse (KERNEL/LOCALONLY are gated on __IIG in OSObject.h).
# -Wno-deprecated-declarations here too: iig parses this file itself and emits the
# same deprecation diagnostics as clang. See DK_CXXFLAGS for why they are expected.
IIG_FLAGS    := -D__IIG=1 -x c++ -std=gnu++17 -isysroot $(DK_SDK) -Wno-deprecated-declarations

# -Wno-deprecated-declarations is deliberate. Source-level #pragma bracketing does work
# for the declarations -- the SDK header does exactly that around its own -- but iig also
# emits the affected declarations inside `#define ..._Methods \` continuations, where a
# pragma cannot go, so a blanket flag is the practical option rather than the only one.
#
# THE COST, worth knowing before M5: this suppresses the warning across the whole dext, so
# an accidental use of a deprecated API will not be flagged. That matters here because the
# deprecated/live split is this project's recurring trap, and several M5-path methods are
# deprecated: IOUserNetworkPacketBufferPool::Create, DeallocatePacket, DeallocatePackets
# and SetTxPacketHeadroom. Check those by hand against the headers.
#
# IOUserNetworkEthernet declares nine methods that are simultaneously
# __deprecated and pure virtual, so a subclass MUST override them to instantiate at
# all, and two of them name deprecated types (IOUserNetworkMACAddress,
# IOUserNetworkMediaType) in their signatures. iig then regenerates those
# declarations into build/gen/SMSC95xxDriver.h, which no source-level pragma can
# bracket. The driver otherwise uses the modern lowercase APIs throughout
# (getHardwareAssists, reportLinkStatus, registerEthernetInterface), so this flag is
# not covering up an accidental use of a deprecated call.
DK_CXXFLAGS  := -target $(DK_TARGET) -isysroot $(DK_SDK) -std=gnu++17 \
                -fno-exceptions -fno-rtti -Wall -Wextra -Wno-deprecated-declarations -I$(GEN) -I$(COMMON)

# TRACE=1 compiles in the per-call and per-frame datapath tracing: the DIAG5 family-callback
# trace, the DIAG6 transmit trace and the DIAG7 receive trace. OFF BY DEFAULT, deliberately.
#
# Every one of those lines earned its keep -- between them they found the queue-enable
# ordering, the offload answer, the RX busy loop and both framing bugs -- so they are compiled
# out rather than deleted. But they must not be in a normal build: os_log throttles hard, and
# ~364,000 messages in one burst once caused later attach-time logs to vanish from the store
# entirely, which is worse than having no tracing at all.
#
#   make app                 quiet: errors, refusals, guards and the counter totals only
#   make app TRACE=1         everything, for chasing a datapath bug
#
# The macros compile to `if (0) os_log(...)` when this is unset, so the format strings and
# arguments stay type-checked either way and a stale trace line cannot rot unnoticed.
ifneq ($(strip $(TRACE)),)
DK_CXXFLAGS  += -DSMSC95XX_TRACE=1
endif

DK_CFLAGS    := -target $(DK_TARGET) -isysroot $(DK_SDK) -std=c11 \
                -Wall -Wextra -I$(COMMON)
DK_LDFLAGS   := -target $(DK_TARGET) -isysroot $(DK_SDK) \
                -framework DriverKit -framework USBDriverKit -framework NetworkingDriverKit

APP_CFLAGS   := -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wextra
APP_LDFLAGS  := -framework Foundation -framework SystemExtensions

.PHONY: all dext app install uninstall clean status
all: dext app

# ---- dext ----

# Editing a constant in common/ must rebuild the objects that consumed it. Without
# these prerequisites make relinks unchanged objects and produces a dext built from
# the old values -- signed and staged, with no signal that anything is stale.
COMMON_HDRS  := $(COMMON)/smsc95xx_proto.h $(COMMON)/smsc95xx_regs.h \
                $(COMMON)/smsc95xx_init.h

# The ivar layout is shared by the two translation units that make up SMSC95xxDriver, so
# both objects must rebuild when it changes -- a stale object built against the old layout
# links cleanly and then reads the wrong offsets.
DEXT_HDRS    := $(DEXT_NAME)/$(DEXT_NAME)_ivars.h

$(GEN)/$(DEXT_NAME).h: $(DEXT_NAME)/$(DEXT_NAME).iig
	@mkdir -p $(GEN)
	xcrun -sdk driverkit iig \
	    --def $< \
	    --header $(GEN)/$(DEXT_NAME).h \
	    --impl $(GEN)/$(DEXT_NAME).iig.cpp \
	    --deployment-target 24.0 \
	    -- $(IIG_FLAGS)

# Generated together with the header by the rule above; depending on it rather than
# declaring a second target keeps `make -j` from running iig twice.
$(GEN)/$(DEXT_NAME).iig.cpp: $(GEN)/$(DEXT_NAME).h

$(BUILD)/$(DEXT_NAME).o: $(DEXT_NAME)/$(DEXT_NAME).cpp $(GEN)/$(DEXT_NAME).h $(COMMON_HDRS) $(DEXT_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -I$(DEXT_NAME) -c $< -o $@

# The second translation unit of the same class: the bulk datapath. Shares
# SMSC95xxDriver_IVars with the file above via $(DEXT_HDRS).
$(BUILD)/$(DEXT_NAME)_datapath.o: $(DEXT_NAME)/$(DEXT_NAME)_datapath.cpp $(GEN)/$(DEXT_NAME).h $(COMMON_HDRS) $(DEXT_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -I$(DEXT_NAME) -c $< -o $@

$(BUILD)/$(DEXT_NAME).iig.o: $(GEN)/$(DEXT_NAME).iig.cpp
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -c $< -o $@

$(BUILD)/smsc95xx_proto.o: $(COMMON)/smsc95xx_proto.c $(COMMON_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang $(DK_CFLAGS) -c $< -o $@

# The chip initialisation sequence, shared verbatim with the probe. Compiled as C for
# the dext with no platform headers involved, which is the whole reason it can live in
# common/ and be unit-tested on the host.
$(BUILD)/smsc95xx_init.o: $(COMMON)/smsc95xx_init.c $(COMMON_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang $(DK_CFLAGS) -c $< -o $@

DEV_NAME     := SMSC95xxUSBDevice

$(GEN)/$(DEV_NAME).h: $(DEV_NAME)/$(DEV_NAME).iig
	@mkdir -p $(GEN)
	xcrun -sdk driverkit iig \
	    --def $< \
	    --header $(GEN)/$(DEV_NAME).h \
	    --impl $(GEN)/$(DEV_NAME).iig.cpp \
	    --deployment-target 24.0 \
	    -- $(IIG_FLAGS)

$(GEN)/$(DEV_NAME).iig.cpp: $(GEN)/$(DEV_NAME).h

$(BUILD)/$(DEV_NAME).o: $(DEV_NAME)/$(DEV_NAME).cpp $(GEN)/$(DEV_NAME).h
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -I$(DEV_NAME) -c $< -o $@

$(BUILD)/$(DEV_NAME).iig.o: $(GEN)/$(DEV_NAME).iig.cpp
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -I$(DEV_NAME) -c $< -o $@

DEXT_OBJS := $(BUILD)/$(DEXT_NAME).o $(BUILD)/$(DEXT_NAME).iig.o $(BUILD)/smsc95xx_proto.o \
             $(BUILD)/smsc95xx_init.o \
             $(BUILD)/$(DEV_NAME).o $(BUILD)/$(DEV_NAME).iig.o \
             $(BUILD)/$(DEXT_NAME)_datapath.o

# ---- entitlements ----

# Substitute the Team ID into the templates. An unsubstituted __TEAM_ID__ would sign and
# verify cleanly and only fail at launch, so check for it here rather than let that
# through; and lint after substituting, because it is the generated file that codesign
# reads. Note codesign parses entitlements with AMFIUnserializeXML, which is stricter
# than plutil -- a double hyphen inside an XML comment passes the lint below and then
# fails signing.
$(DEXT_ENT): $(DEXT_NAME)/$(DEXT_NAME).entitlements.in
	@mkdir -p $(GEN)
	sed 's/__TEAM_ID__/$(TEAM_ID)/g' $< > $@
	@! grep -q __TEAM_ID__ $@ || { echo "$@: __TEAM_ID__ not substituted; set TEAM_ID or pass a profile" >&2; rm -f $@; exit 1; }
	plutil -lint $@

$(APP_ENT): $(APP_NAME)/Installer.entitlements.in
	@mkdir -p $(GEN)
	sed 's/__TEAM_ID__/$(TEAM_ID)/g' $< > $@
	@! grep -q __TEAM_ID__ $@ || { echo "$@: __TEAM_ID__ not substituted; set TEAM_ID or pass a profile" >&2; rm -f $@; exit 1; }
	plutil -lint $@

# A dext bundle is flat: Info.plist and the executable sit at the top level,
# with no Contents/MacOS. Verified against /System/Library/DriverExtensions.
dext: $(DEXT_OBJS) $(DEXT_NAME)/Info.plist $(DEXT_ENT)
	plutil -lint $(DEXT_NAME)/Info.plist
	rm -rf $(DEXT_BUNDLE)
	mkdir -p $(DEXT_BUNDLE)
	xcrun -sdk driverkit clang++ $(DK_LDFLAGS) -o $(DEXT_BUNDLE)/$(DEXT_ID) $(DEXT_OBJS)
	cp $(DEXT_NAME)/Info.plist $(DEXT_BUNDLE)/Info.plist
	@# A dext bundle is flat, so the profile goes at its root. This location is NOT
	@# documented by Apple and is the one part of the signing setup taken on trust --
	@# verify with `codesign -d -vvv` that the entitlements read as profile-backed.
	$(if $(DEXT_PROFILE),cp "$(DEXT_PROFILE)" $(DEXT_BUNDLE)/embedded.provisionprofile,@true)
	codesign --force --sign "$(CODESIGN_ID)" \
	    --entitlements $(DEXT_ENT) \
	    --identifier $(DEXT_ID) \
	    --timestamp=none $(DEXT_BUNDLE)
	codesign -v --verbose=2 $(DEXT_BUNDLE)

# ---- installer app ----

# .icns from the 1024x1024 master. iconutil does not scale, so every size is
# derived explicitly with sips; the file names inside the .iconset are fixed by
# iconutil and must match exactly.
ICON_SRC     := $(APP_NAME)/AppIcon.png
ICONSET      := $(BUILD)/AppIcon.iconset
ICNS         := $(BUILD)/AppIcon.icns

$(ICNS): $(ICON_SRC)
	rm -rf $(ICONSET)
	@mkdir -p $(ICONSET)
	set -e; for sz in 16 32 128 256 512; do \
	    sips -z $$sz $$sz $(ICON_SRC) --out $(ICONSET)/icon_$${sz}x$${sz}.png >/dev/null; \
	    sips -z $$((sz * 2)) $$((sz * 2)) $(ICON_SRC) --out $(ICONSET)/icon_$${sz}x$${sz}@2x.png >/dev/null; \
	done
	iconutil -c icns $(ICONSET) -o $(ICNS)

app: dext $(ICNS) $(APP_NAME)/main.m $(APP_NAME)/Info.plist $(APP_ENT)
	plutil -lint $(APP_NAME)/Info.plist
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources $(APP_BUNDLE)/Contents/Library/SystemExtensions
	xcrun clang $(APP_CFLAGS) $(APP_LDFLAGS) -o $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME) $(APP_NAME)/main.m
	cp $(APP_NAME)/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	cp $(ICNS) $(APP_BUNDLE)/Contents/Resources/AppIcon.icns
	cp -R $(DEXT_BUNDLE) $(APP_BUNDLE)/Contents/Library/SystemExtensions/
	$(if $(APP_PROFILE),cp "$(APP_PROFILE)" $(APP_BUNDLE)/Contents/embedded.provisionprofile,@true)
	codesign --force --sign "$(CODESIGN_ID)" \
	    --entitlements $(APP_ENT) \
	    --identifier $(APP_ID) \
	    --timestamp=none $(APP_BUNDLE)
	codesign -v --verbose=2 $(APP_BUNDLE)

# System extensions are only accepted from an app in /Applications.
install: app
	rm -rf /Applications/$(APP_NAME).app
	cp -R $(APP_BUNDLE) /Applications/
	/Applications/$(APP_NAME).app/Contents/MacOS/$(APP_NAME) activate

uninstall:
	-/Applications/$(APP_NAME).app/Contents/MacOS/$(APP_NAME) deactivate
	rm -rf /Applications/$(APP_NAME).app

# `codesign -v` reports "valid on disk" even when signing silently fell back to ad-hoc,
# so check the Authority chain, not validity.
.PHONY: signinfo
signinfo:
	@echo "--- dext ---"
	@codesign -dv --verbose=2 $(DEXT_BUNDLE) 2>&1 | grep -E "Authority|TeamIdentifier|Signature|flags" || true
	@test -f $(DEXT_BUNDLE)/embedded.provisionprofile && echo "  profile: embedded" || echo "  profile: NONE"
	@echo "--- installer app ---"
	@codesign -dv --verbose=2 $(APP_BUNDLE) 2>&1 | grep -E "Authority|TeamIdentifier|Signature|flags" || true
	@test -f $(APP_BUNDLE)/Contents/embedded.provisionprofile && echo "  profile: embedded" || echo "  profile: NONE"

status:
	systemextensionsctl list
	@echo "--- running vs staged ---"
	@# systemextensionsctl reports the newly activated version as "activated
	@# enabled" even when the kernel is still executing the previous one, because
	@# a dext holding a device open does not terminate on upgrade. Compare the two
	@# directly, or risk testing a stale binary while believing otherwise.
	@run=$$(ps -Ao args | grep '[S]ystemExtensions/.*smsc95xx' | head -1); \
	if [ -z "$$run" ]; then echo "  running: (none)"; else \
	    d=$$(echo "$$run" | sed 's|\(/Library/SystemExtensions/[^/]*\)/.*|\1|'); \
	    echo "  running: v$$(plutil -extract CFBundleVersion raw $$d/*.dext/Info.plist 2>/dev/null) [$$(basename $$d)]"; \
	fi
	@for d in /Library/SystemExtensions/*/; do \
	    f=$$(find "$$d" -maxdepth 1 -name '*.dext' 2>/dev/null | head -1); \
	    [ -n "$$f" ] && echo "  staged:  v$$(plutil -extract CFBundleVersion raw "$$f/Info.plist" 2>/dev/null) [$$(basename $$d)]"; \
	done; true
	@echo "  declared in tree: v$$(plutil -extract CFBundleVersion raw $(DEXT_NAME)/Info.plist)"
	@echo "--- matched? ---"
	@# Search the USB device subtree, not `-c IOUserService`: that class filter
	@# returns nothing for this driver and reads as a false negative. The device
	@# is also nameless when the EEPROM auto-load fails, so do not match on its
	@# product name either.
	@ioreg -w0 -r -c IOUSBHostDevice -l | grep -E 'SMSC95xxDriver|"SMSC95xx[A-Za-z]+"' \
	    || echo "  (not matched -- no SMSC95xxDriver node under any USB device)"

clean:
	rm -rf $(BUILD)
