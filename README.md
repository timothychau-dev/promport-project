<div align="center">

# Promport

**Don't prompt alone, prompt together.**

A personal fork of [Passport OS](https://github.com/manchunx7-bit/passport-os) for the FoloToy AI Passport · ESP32-C3 · 8 MB Flash

by Timothy Chau · [Prompt in Progress](https://promptinprogress.com) · [createwithtim.com/links](https://createwithtim.com/links/)

</div>

---

## What is this?

Promport — **Prom**pt + Pass**port** — is my day-to-day build of the FoloToy AI Passport hardware: a pocket-sized profile card and presenter remote, rebranded to the Prompt in Progress look — paper `#F3F1E9`, navy `#0D0E2B`, teal `#00C2C7`, Bebas Neue display, mono labels.

It is a trimmed-down fork of the Passport OS community firmware. Most feature apps were removed; what's left is what I actually use:

| App | What it does |
| --- | --- |
| **Home (Profile)** | Centered avatar, name, and brand tagline. Profile (avatar/name/photo) is set by uploading from your phone during setup. |
| **Prompter** (presenter) | BLE HID keyboard remote for slideshows — start/advance/back presentations on a paired computer. |
| Launcher, Wi-Fi, Settings | System apps for app switching, network setup, and device settings. |
| Stopwatch, Pomodoro | Kept from upstream; available from the launcher. |

The boot splash and Wi-Fi setup hotspot are rebranded (`PROMPORT-WiFi-xxxx`).

## The story

Promport started as an adaptation of the Passport OS community firmware, built for one job: helping me run events and talks — introduce myself, then control my slides without fumbling for a laptop or a separate clicker. I liked the hardware and the shell — the launcher, Wi-Fi provisioning, settings, and battery management all work well — but for that job I only needed two things: a way to show who I am, and a way to drive my presentation. So the ten-app build was trimmed down to what I actually carry, and the whole interface was re-skinned in the Prompt in Progress brand.

What I actually did on top of upstream:

- **AI-assisted English translation** of the firmware's user-facing text (upstream is Chinese-first).
- **Layout updates** — a redesigned centered Home page (avatar, name, tagline) and a rebranded boot splash.
- **Rebrand** to Prompt in Progress: paper `#F3F1E9`, navy `#0D0E2B`, teal `#00C2C7`, Bebas Neue display, mono labels.

The Prompter app comes unchanged from [ai-passport-ppt](https://github.com/YeatsLiao/ai-passport-ppt) — a Bluedroid + `esp_hid` BLE HID keyboard (`PPT-Remote`) for the same FoloToy AI Passport hardware, tested against PowerPoint and Keynote, with a talk timer, slideshow-start shortcut, and a pairing-reset combo. Together with the Profile home it covers the full event flow: the device introduces you on stage, then becomes your clicker for the slides.

## Upstream

Everything here is built on the excellent work of the [Passport OS](https://github.com/manchunx7-bit/passport-os) community firmware — see that repo for the full 10-app original, related projects (Muse, AI voice input), and community install guides. The Prompter app is ported from [YeatsLiao/ai-passport-ppt](https://github.com/YeatsLiao/ai-passport-ppt).

## Building from source

Requires **Git** and **ESP-IDF 5.5.3** with its toolchain.

```bash
git clone https://github.com/timothychau-dev/promport-project.git
cd promport-project
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
