CXX ?= clang++
WEBRTC_DIR ?= /Library/Frameworks
WEBRTC_FRAMEWORK ?= WebRTC
OPENSSL_PREFIX ?= $(shell brew --prefix openssl 2>/dev/null || echo /usr/local/opt/openssl)
APP = crossremote-agent

SRC = src/main.mm ../common/cr_e2ee.cpp
HEADERS = ../common/cr_proto.h ../common/cr_e2ee.h

CXXFLAGS = -std=c++17 -fobjc-arc -O2 -Wall \
  -I../common \
  -I$(OPENSSL_PREFIX)/include \
  -F$(WEBRTC_DIR) \
  -framework $(WEBRTC_FRAMEWORK) \
  -framework Cocoa -framework CoreGraphics -framework CoreMedia \
  -framework CoreVideo -framework VideoToolbox -framework AVFoundation \
  -framework AudioToolbox -framework Foundation
LDFLAGS = -L$(OPENSSL_PREFIX)/lib -lcrypto \
  -Wl,-rpath,@executable_path/../Frameworks -Wl,-rpath,$(WEBRTC_DIR)

all: $(APP)

$(APP): $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(APP) $(LDFLAGS)

bundle: $(APP)
	mkdir -p CrossRemote.app/Contents/MacOS CrossRemote.app/Contents/Resources CrossRemote.app/Contents/Frameworks
	cp $(APP) CrossRemote.app/Contents/MacOS/
	cp Info.plist CrossRemote.app/Contents/
	if [ -d "$(WEBRTC_DIR)/$(WEBRTC_FRAMEWORK).framework" ]; then \
	  cp -R "$(WEBRTC_DIR)/$(WEBRTC_FRAMEWORK).framework" CrossRemote.app/Contents/Frameworks/; \
	fi
	codesign --force --deep --sign - CrossRemote.app || true
	du -sh CrossRemote.app
	ls CrossRemote.app/Contents CrossRemote.app/Contents/Frameworks || true

clean:
	rm -f $(APP)
	rm -rf CrossRemote.app

.PHONY: all bundle clean
