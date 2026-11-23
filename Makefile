# Builds BatteryHub.exe with MinGW-w64 (MSYS2 MINGW64/UCRT64 shell, or any mingw g++ on PATH).
CXX      ?= g++
WINDRES  ?= windres
VERSION  ?= 0.0.0
CHANNEL  ?= dev
.DEFAULT_GOAL := BatteryHub.exe
CXXFLAGS ?= -std=c++17 -Os -Wall -Wno-missing-field-initializers -ffunction-sections -fdata-sections \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
LDFLAGS  ?= -municode -mwindows -static -s -Wl,--gc-sections
LIBS     := -lhid -lsetupapi -lcfgmgr32 -lole32 -loleaut32 -lcomctl32 -lgdi32 -lruntimeobject -lws2_32 -lbluetoothapis -ld2d1 -ldwrite -ldwmapi

SRC := $(wildcard src/*.cpp) $(wildcard src/drivers/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) build/app_res.o

BatteryHub.exe: $(OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

build/%.o: src/%.cpp $(wildcard src/*.h src/drivers/*.h) build/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -municode -c $< -o $@

build/version.h: scripts/generate-version.ps1 FORCE
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-version.ps1 -Version "$(VERSION)" -Channel "$(CHANNEL)"

# Every lang/*.txt is a built-in UI language: joined into one resource, each after an "@file <name>" line.
build/languages.txt: $(wildcard lang/*.txt)
	@mkdir -p build
	@printf '' > $@; for f in $^; do printf '@file %s\n' "$$(basename $$f .txt)" >> $@; cat "$$f" >> $@; printf '\n' >> $@; done

build/app_res.o: res/app.rc res/app.manifest res/app.ico res/app_light.ico build/languages.txt build/version.h
	@mkdir -p build
	$(WINDRES) -i $< -o $@ --include-dir res --include-dir build

clean:
	rm -rf build BatteryHub.exe

test:
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall tests/device_aliases_test.cpp -o build/device_aliases_test.exe
	./build/device_aliases_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/alert_rules_test.cpp -o build/alert_rules_test.exe
	./build/alert_rules_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/history_model_test.cpp -o build/history_model_test.exe
	./build/history_model_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra tests/runtime_estimate_test.cpp -o build/runtime_estimate_test.exe
	./build/runtime_estimate_test.exe
	$(CXX) $(CXXFLAGS) tests/aula_mouse_driver_test.cpp src/drivers/aula_mouse.cpp -o build/aula_mouse_driver_test.exe
	./build/aula_mouse_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/darkrim_driver_test.cpp src/drivers/darkrim.cpp -o build/darkrim_driver_test.exe
	./build/darkrim_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/nuphy_driver_test.cpp src/drivers/nuphy.cpp -o build/nuphy_driver_test.exe
	./build/nuphy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/maono_driver_test.cpp src/drivers/maono.cpp -o build/maono_driver_test.exe
	./build/maono_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/astro_driver_test.cpp src/drivers/astro.cpp -o build/astro_driver_test.exe
	./build/astro_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/steelseries_mouse_test.cpp src/drivers/steelseries.cpp -o build/steelseries_mouse_test.exe
	./build/steelseries_mouse_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/rapoo_protocol_test.cpp -o build/rapoo_protocol_test.exe
	./build/rapoo_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/rapoo_driver_test.cpp src/drivers/rapoo.cpp -o build/rapoo_driver_test.exe
	./build/rapoo_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/rawm_protocol_test.cpp -o build/rawm_protocol_test.exe
	./build/rawm_protocol_test.exe
	$(CXX) $(CXXFLAGS) tests/rawm_driver_test.cpp src/drivers/rawm.cpp -o build/rawm_driver_test.exe
	./build/rawm_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_driver_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_driver_test.exe
	./build/ninjutso_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/cherry_driver_test.cpp src/drivers/cherry.cpp -o build/cherry_driver_test.exe
	./build/cherry_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/cherry_xtrfy_driver_test.cpp src/drivers/cherry.cpp -o build/cherry_xtrfy_driver_test.exe
	./build/cherry_xtrfy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/ninjutso_cable_test.cpp src/drivers/ninjutso.cpp -o build/ninjutso_cable_test.exe
	./build/ninjutso_cable_test.exe
	$(CXX) $(CXXFLAGS) tests/machenike_driver_test.cpp src/drivers/machenike.cpp -o build/machenike_driver_test.exe
	./build/machenike_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/niceboy_driver_test.cpp src/drivers/niceboy.cpp -o build/niceboy_driver_test.exe
	./build/niceboy_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/redragon_evision_driver_test.cpp src/drivers/redragon_evision.cpp -o build/redragon_evision_driver_test.exe
	./build/redragon_evision_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/alienware_driver_test.cpp src/drivers/alienware.cpp -o build/alienware_driver_test.exe
	./build/alienware_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/mchose_modern_driver_test.cpp src/drivers/mchose_modern.cpp -o build/mchose_modern_driver_test.exe
	./build/mchose_modern_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/vgn_driver_test.cpp src/drivers/vgn.cpp -o build/vgn_driver_test.exe
	./build/vgn_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/vaxee_driver_test.cpp src/drivers/vaxee.cpp -o build/vaxee_driver_test.exe
	./build/vaxee_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/finalmouse_driver_test.cpp src/drivers/finalmouse.cpp -o build/finalmouse_driver_test.exe
	./build/finalmouse_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/keychron_driver_test.cpp src/drivers/keychron.cpp -o build/keychron_driver_test.exe
	./build/keychron_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_haste_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_haste_driver_test.exe
	./build/hyperx_haste_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_cloud_flight_s_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_cloud_flight_s_driver_test.exe
	./build/hyperx_cloud_flight_s_driver_test.exe
	$(CXX) $(CXXFLAGS) tests/aula_sc580_test.cpp src/drivers/aula.cpp -o build/aula_sc580_test.exe
	./build/aula_sc580_test.exe
	$(CXX) $(CXXFLAGS) tests/hyperx_stinger_driver_test.cpp src/drivers/hyperx.cpp -o build/hyperx_stinger_driver_test.exe
	./build/hyperx_stinger_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/galaxy_buds_protocol_test.cpp -o build/galaxy_buds_protocol_test.exe
	./build/galaxy_buds_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/logitech_g435_0adf_test.cpp -o build/logitech_g435_0adf_test.exe
	./build/logitech_g435_0adf_test.exe
	$(CXX) -std=c++17 -O2 -Wall -Isrc tests/xenta_protocol_test.cpp src/drivers/xenta_protocol.cpp -o build/xenta_protocol_test.exe
	./build/xenta_protocol_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/labels_test.cpp src/labels.cpp -o build/labels_test.exe
	./build/labels_test.exe
	$(CXX) $(CXXFLAGS) tests/brands_test.cpp src/brands.cpp -o build/brands_test.exe
	./build/brands_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/xbox_bluetooth_identity_test.cpp -o build/xbox_bluetooth_identity_test.exe
	./build/xbox_bluetooth_identity_test.exe
	$(CXX) $(CXXFLAGS) tests/jabra_driver_test.cpp src/drivers/jabra.cpp -o build/jabra_driver_test.exe
	./build/jabra_driver_test.exe
	$(CXX) -std=c++17 -O2 -Wall tests/gamesir_ble_protocol_test.cpp -o build/gamesir_ble_protocol_test.exe
	./build/gamesir_ble_protocol_test.exe


FORCE:

.PHONY: clean test FORCE
