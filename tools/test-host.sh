#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# macOS ld does not support --gc-sections; the covered code paths are exercised on Linux.
if [ "$(uname -s)" = "Darwin" ]; then GC_SECTIONS_FLAG=; fi
test_dir="$(mktemp -d /tmp/passport-host.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -Itests/registry_mocks -Imain \
    tests/test_app_registry.cc -o "$test_dir/registry"
"$test_dir/registry"
python3 tools/generate_ui_sfx.py --check
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/game_audio_mocks -Imain tests/test_ui_sfx.c -lm -o "$test_dir/ui-sfx"
"$test_dir/ui-sfx"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_terminal.cc -o "$test_dir/terminal"
"$test_dir/terminal"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/audio_mocks -Icomponents/bsp/src \
    tests/test_audio_volume.c components/bsp/src/bsp_audio_volume.c -lm -o "$test_dir/audio-volume"
"$test_dir/audio-volume"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_stopwatch.c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch"
"$test_dir/stopwatch"
python3 tests/test_wifi_configure.py
python3 tests/test_wifi_internet.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_battery_guard.c -o "$test_dir/battery-guard"
"$test_dir/battery-guard"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_pomodoro.c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro"
"$test_dir/pomodoro"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro-core.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch-core.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_timer_resume.cc "$test_dir/stopwatch-core.o" "$test_dir/pomodoro-core.o" -o "$test_dir/timer-resume"
"$test_dir/timer-resume"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable -Wno-misleading-indentation \
    -fsanitize=address,undefined -g -Itests/pomodoro_mocks -Imain -Imain/fonts \
    tests/test_pomodoro_app.cc "$test_dir/pomodoro-core.o" -o "$test_dir/pomodoro-app"
"$test_dir/pomodoro-app"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_wifi_keyboard.cc -o "$test_dir/wifi-keyboard"
"$test_dir/wifi-keyboard"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-sign-compare \
    -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -g \
    -Itests/wifi_mocks -Icomponents/esp-wifi-connect/include \
    tests/test_wifi_station.cc -o "$test_dir/wifi-station"
"$test_dir/wifi-station"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain -Icomponents/esp-wifi-connect/include \
    tests/test_captive_setup.cc -o "$test_dir/captive-setup"
"$test_dir/captive-setup"
node tests/test_profile_page.js
node tests/test_portrait_pixels.js
python3 tests/test_card_gallery.py
python3 tests/test_avatar_upload.py
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -ffunction-sections -fdata-sections \
    -Itests/home_mocks -Itests/radio_mocks -Imain \
    tests/test_home_storage.c ${GC_SECTIONS_FLAG--Wl,--gc-sections} -lm -o "$test_dir/home-storage"
"$test_dir/home-storage"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Wno-deprecated-declarations \
    -Itests/vendor/cjson -c tests/vendor/cjson/cJSON.c -o "$test_dir/cjson.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/vendor/cjson -Icomponents/esp-wifi-connect/include \
    tests/test_phone_time.cc "$test_dir/cjson.o" -o "$test_dir/phone-time"
"$test_dir/phone-time"
python3 tests/test_city_catalog.py
python3 tests/test_home_weather.py
python3 tests/test_home_weather_schedule.py
python3 tests/test_input_serial_dispatch.py
python3 tests/test_button_poll.py
python3 tests/test_bsp_button.py
python3 tests/test_bsp_audio_profiles.py
python3 tests/test_memory_telemetry.py
python3 tests/test_home_portrait_lifecycle.py
python3 tests/test_verify_firmware.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/battery_mocks -Icomponents/bsp/include \
    tests/test_battery_driver.c components/bsp/src/bsp_battery.c -o "$test_dir/battery"
"$test_dir/battery"
echo "Host regression tests: PASS"
