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

BUILD        := build
# The bundle and its executable must be named after the bundle IDENTIFIER, not the
# project name. Named SMSC95xxDriver.dext instead, OSSystemExtensionRequest fails
# with OSSystemExtensionErrorDomain code 4, "Unable to find any matched extension
# with identifier ...", even though the Info.plist identifier is correct. Apple's
# own dexts in /System/Library/DriverExtensions follow the same convention.
DEXT_BUNDLE  := $(BUILD)/$(DEXT_ID).dext
APP_BUNDLE   := $(BUILD)/$(APP_NAME).app
GEN          := $(BUILD)/gen

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
IIG_FLAGS    := -D__IIG=1 -x c++ -std=gnu++17 -isysroot $(DK_SDK)

DK_CXXFLAGS  := -target $(DK_TARGET) -isysroot $(DK_SDK) -std=gnu++17 \
                -fno-exceptions -fno-rtti -Wall -Wextra -I$(GEN) -I$(COMMON)
DK_CFLAGS    := -target $(DK_TARGET) -isysroot $(DK_SDK) -std=c11 \
                -Wall -Wextra -I$(COMMON)
DK_LDFLAGS   := -target $(DK_TARGET) -isysroot $(DK_SDK) \
                -framework DriverKit -framework USBDriverKit

APP_CFLAGS   := -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wextra
APP_LDFLAGS  := -framework Foundation -framework SystemExtensions

.PHONY: all dext app install uninstall clean status
all: dext app

# ---- dext ----

# Editing a constant in common/ must rebuild the objects that consumed it. Without
# these prerequisites make relinks unchanged objects and produces a dext built from
# the old values -- signed and staged, with no signal that anything is stale.
COMMON_HDRS  := $(COMMON)/smsc95xx_proto.h $(COMMON)/smsc95xx_regs.h

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

$(BUILD)/$(DEXT_NAME).o: $(DEXT_NAME)/$(DEXT_NAME).cpp $(GEN)/$(DEXT_NAME).h $(COMMON_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -c $< -o $@

$(BUILD)/$(DEXT_NAME).iig.o: $(GEN)/$(DEXT_NAME).iig.cpp
	xcrun -sdk driverkit clang++ $(DK_CXXFLAGS) -c $< -o $@

$(BUILD)/smsc95xx_proto.o: $(COMMON)/smsc95xx_proto.c $(COMMON_HDRS)
	@mkdir -p $(BUILD)
	xcrun -sdk driverkit clang $(DK_CFLAGS) -c $< -o $@

DEXT_OBJS := $(BUILD)/$(DEXT_NAME).o $(BUILD)/$(DEXT_NAME).iig.o $(BUILD)/smsc95xx_proto.o

# A dext bundle is flat: Info.plist and the executable sit at the top level,
# with no Contents/MacOS. Verified against /System/Library/DriverExtensions.
dext: $(DEXT_OBJS) $(DEXT_NAME)/Info.plist $(DEXT_NAME)/$(DEXT_NAME).entitlements
	plutil -lint $(DEXT_NAME)/Info.plist
	plutil -lint $(DEXT_NAME)/$(DEXT_NAME).entitlements
	rm -rf $(DEXT_BUNDLE)
	mkdir -p $(DEXT_BUNDLE)
	xcrun -sdk driverkit clang++ $(DK_LDFLAGS) -o $(DEXT_BUNDLE)/$(DEXT_ID) $(DEXT_OBJS)
	cp $(DEXT_NAME)/Info.plist $(DEXT_BUNDLE)/Info.plist
	codesign --force --sign - \
	    --entitlements $(DEXT_NAME)/$(DEXT_NAME).entitlements \
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

app: dext $(ICNS) $(APP_NAME)/main.m $(APP_NAME)/Info.plist $(APP_NAME)/Installer.entitlements
	plutil -lint $(APP_NAME)/Info.plist
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources $(APP_BUNDLE)/Contents/Library/SystemExtensions
	xcrun clang $(APP_CFLAGS) $(APP_LDFLAGS) -o $(APP_BUNDLE)/Contents/MacOS/$(APP_NAME) $(APP_NAME)/main.m
	cp $(APP_NAME)/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	cp $(ICNS) $(APP_BUNDLE)/Contents/Resources/AppIcon.icns
	cp -R $(DEXT_BUNDLE) $(APP_BUNDLE)/Contents/Library/SystemExtensions/
	codesign --force --sign - \
	    --entitlements $(APP_NAME)/Installer.entitlements \
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
