<div align="center">

# Prompter

**Don't prompt alone, prompt together.**

A personal fork of [Passport OS](https://github.com/manchunx7-bit/passport-os) for the FoloToy AI Passport · ESP32-C3 · 8 MB Flash

by Timothy Chau · [Prompt in Progress](https://promptinprogress.com) · [createwithtim.com/links](https://createwithtim.com/links/)

</div>

---

## What is this?

Prompter is my day-to-day build of the FoloToy AI Passport hardware: a pocket-sized profile card and presenter remote, rebranded to the Prompt in Progress look — paper `#F3F1E9`, navy `#0D0E2B`, teal `#00C2C7`, Bebas Neue display, mono labels.

It is a trimmed-down fork of the Passport OS community firmware. Most feature apps were removed; what's left is what I actually use:

| App | What it does |
| --- | --- |
| **Home (Profile)** | Centered avatar, name, and brand tagline. Profile (avatar/name/photo) is set by uploading from your phone during setup. |
| **Presenter** | BLE HID keyboard remote for slideshows — start/advance/back presentations on a paired computer. |
| Launcher, Wi-Fi, Settings | System apps for app switching, network setup, and device settings. |
| Stopwatch, Pomodoro | Kept from upstream; available from the launcher. |

The boot splash and Wi-Fi setup hotspot are rebranded (`PROMPORT-WiFi-xxxx`).

## Upstream

Everything here is built on the excellent work of the [Passport OS](https://github.com/manchunx7-bit/passport-os) community firmware — see that repo for the full 10-app original, related projects (Muse, AI voice input), and community install guides.

## Building from source

Requires **Git** and **ESP-IDF 5.5.3** with its toolchain.

```bash
git clone https://github.com/timothychau-dev/prompter-project.git
cd prompter-project
bash build.sh release
```

The build script loads ESP-IDF from `/opt/esp-idf` by default; point `IDF_DIR` elsewhere if needed:

```bash
IDF_DIR="$HOME/esp/esp-idf" bash build.sh release
```

Build outputs:

| File | Use |
| --- | --- |
| `build-public/FoloToy-AI-Passport-full.bin` | Full image, flash at `0x0` |
| `build-public/passport-os.bin` | App image, flash at `0x10000` (same partition layout only) |

### Validate before shipping

With ESP-IDF 5.5.3 activated:

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
```

The second command rebuilds and verifies the merged image at `build/FoloToy-AI-Passport-full.bin`. Always check buttons, display, audio, and Wi-Fi setup on real hardware after a build.

### Repo map

| What | Where |
| --- | --- |
| App UI/logic | [`main/apps/`](main/apps/) |
| Launcher, setup, system interaction | [`main/launcher/`](main/launcher/) |
| UI resources & fonts | [`main/ui/`](main/ui/) · [`main/fonts/`](main/fonts/) |
| Board drivers (buttons, display, audio, battery) | [`components/bsp/`](components/bsp/) |
| Wi-Fi provisioning & connection | [`components/esp-wifi-connect/`](components/esp-wifi-connect/) |
| Host tests & build tools | [`tests/`](tests/) · [`tools/`](tools/) |

## Flashing

Applies to the matching FoloToy AI Passport hardware (ESP32-C3, 8 MB Flash). A full image (`*-full.bin` at `0x0`) overwrites everything in its range — for a full reset, erase the flash first. To keep existing Wi-Fi/profile data, confirm the partition layout matches and use the app image at `0x10000` instead.

## License

Project under the [MIT license](LICENSE). Third-party components (Muse, Noise protocol, and others) keep their own `LICENSE`/`NOTICE` terms in their directories; the Jollybot character usage terms are in [`main/apps/muse/JOLLYBOT-NOTICE.md`](main/apps/muse/JOLLYBOT-NOTICE.md).

Thanks to FoloToy, the upstream Passport OS project, and everyone who shared feedback along the way.
