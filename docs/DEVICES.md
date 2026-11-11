# Compatible devices

Which phones and tablets run Galar Native (Pokémon Sword and Shield), from players' reports and our own tests. Last updated for **v0.67** (November 2026).

What matters most is the **graphics chip (GPU)**, more than the phone's brand. To find yours, look up your phone's processor: Snapdragon chips have an **Adreno** GPU, Samsung Exynos chips an **Xclipse** (or Mali on older ones), MediaTek Dimensity and Helio chips a **Mali** (some cheaper ones a PowerVR), Google Tensor G1–G4 a **Mali**, Tensor G5 a **PowerVR**.

| Status | Meaning |
|---|---|
| ✅ Plays | Plays through the game at about 30 fps |
| 🟡 Plays with problems | Runs, but slowly or with a known bug |
| 🔵 Plays with another driver | Doesn't run with the phone's own graphics driver, but does with a custom one (Menu → System → Graphics driver) |
| ❌ Doesn't run yet | Crashes or shows a white screen; being worked on |

**What every device needs:** Android 12 or newer, a 64-bit phone, enough free space for the game (about 15 GB once set up, more while it's being set up), and your own game dump and keys. 8 GB of RAM or more is best; HD texture mods need 12 GB.

## Adreno (Qualcomm Snapdragon): best supported

| Device | Chip | GPU | Status | Notes |
|---|---|---|---|---|
| Samsung Galaxy S25+ | Snapdragon 8 Elite | Adreno 830 | ✅ Plays | Our test phone. 30 fps; 60 fps option; frame generation up to 4x (120 fps on its screen) |
| POCO F6 | Snapdragon 8s Gen 3 | Adreno 735 | ✅ Plays | Played through the story ([#144](https://github.com/simipoB/Galar-Native-Android/issues/144): credits freeze fixed in v0.67) |
| Samsung Galaxy S23 | Snapdragon 8 Gen 2 | Adreno 740 | ✅ Plays | Some menus stretch with "Faster rendering" on ([#139](https://github.com/simipoB/Galar-Native-Android/issues/139)); leave it off |
| Motorola Edge 50 | Snapdragon 7 Gen 1 | Adreno 644 | ✅ Plays | Runs well at the lowest graphics setting ([#143](https://github.com/simipoB/Galar-Native-Android/issues/143)) |
| Xiaomi Mi 10T / 10T Pro | Snapdragon 865 | Adreno 650 | ✅ Plays | Reported with a Turnip driver ([#80](https://github.com/simipoB/Galar-Native-Android/issues/80)); setup's shader step takes hours on this chip |
| vivo V40 Lite 5G | Snapdragon 4 Gen 2 (SM4450) | Adreno 613 | 🟡 Plays with problems | Too slow for 30 fps: the chip's CPU is the limit ([#99](https://github.com/simipoB/Galar-Native-Android/issues/99)) |
| POCO X3 Pro | Snapdragon 860 | Adreno 640 | ❌ Doesn't run yet | Closes on start ([#58](https://github.com/simipoB/Galar-Native-Android/issues/58)) |
| Motorola Edge 30 | Snapdragon 778G+ | Adreno 642L | ❌ Doesn't run yet | Closes after setup ([#87](https://github.com/simipoB/Galar-Native-Android/issues/87)) |
| Redmi Note 13 Pro 5G | Snapdragon 7s Gen 2 | Adreno 710 | ❌ Doesn't run yet | Black screen ([#102](https://github.com/simipoB/Galar-Native-Android/issues/102)) |

## Xclipse (Samsung Exynos)

| Device | Chip | GPU | Status | Notes |
|---|---|---|---|---|
| Samsung Galaxy A56 | Exynos 1580 | Xclipse 540 | ✅ Plays | Since v0.47 ([#45](https://github.com/simipoB/Galar-Native-Android/issues/45)) |
| Samsung Galaxy A55 | Exynos 1480 | Xclipse 530 | 🟡 Plays with problems | Plays; Max Raid Battles can crash ([#115](https://github.com/simipoB/Galar-Native-Android/issues/115)) |
| Samsung Galaxy S24 FE | Exynos 2400e | Xclipse 940 | 🟡 Plays with problems | Crashes and lag after a One UI update; being worked on ([#41](https://github.com/simipoB/Galar-Native-Android/issues/41), [#54](https://github.com/simipoB/Galar-Native-Android/issues/54)) |

## Mali (MediaTek Dimensity / Helio, Google Tensor G1–G4)

Arm's own Mali graphics driver either closes the game a few frames in or draws it white. We're tracking that down with players' help ([#158](https://github.com/simipoB/Galar-Native-Android/issues/158): if you have a Mali phone, the test there takes a minute). Until then, the newer Malis can play with **faux123's PanVK** driver; the older ones (G57, G68, G77, G78, G52) with **FristOneRR's PanVK**, which the app suggests itself.

| Device | Chip | GPU | Status | Notes |
|---|---|---|---|---|
| POCO X8 Pro Max | Dimensity 9500s | Immortalis-G925 MC11 | 🔵 Plays with another driver | Plays with faux123's PanVK (Pixel 7 / Mali-G710 build); white with Arm's driver ([#111](https://github.com/simipoB/Galar-Native-Android/issues/111)) |
| POCO X6 Pro | Dimensity 8300 Ultra | Mali-G615 MC6 | 🔵 Plays with another driver | Playable with faux123's PanVK 1.1.6; white with Arm's driver ([#47](https://github.com/simipoB/Galar-Native-Android/issues/47), [#156](https://github.com/simipoB/Galar-Native-Android/issues/156)) |
| Phones with Dimensity 9400 | Dimensity 9400 | Immortalis-G925 | ❌ Doesn't run yet | White screen with Arm's driver ([#69](https://github.com/simipoB/Galar-Native-Android/issues/69)); faux123's PanVK is worth a try |
| POCO X7 Pro, Infinix GT 50 Pro | Dimensity 8400 and others | Mali-G720 MC7 | ❌ Doesn't run yet | White screen ([#85](https://github.com/simipoB/Galar-Native-Android/issues/85), [#128](https://github.com/simipoB/Galar-Native-Android/issues/128)) |
| vivo X100 Pro, Galaxy Tab S10+ | Dimensity 9300 / 9300+ | Immortalis-G720 MC12 | ❌ Doesn't run yet | White screen or a crash a few frames in ([#98](https://github.com/simipoB/Galar-Native-Android/issues/98), [#155](https://github.com/simipoB/Galar-Native-Android/issues/155)) |
| Phones with Dimensity 7200 / 8200 (vivo V27 and others) | Dimensity 7200 / 8200 | Mali-G610 MC4 / MC6 | ❌ Doesn't run yet | White screen ([#79](https://github.com/simipoB/Galar-Native-Android/issues/79), [#88](https://github.com/simipoB/Galar-Native-Android/issues/88)) |
| Moto g86, Motorola edge 60 fusion, vivo V2437, Galaxy Tab A11+, other Dimensity 7300 / 7400 phones | Dimensity 7300 / 7400 | Mali-G615 MC2 | ❌ Doesn't run yet | Closes three frames in with Arm's driver ([#82](https://github.com/simipoB/Galar-Native-Android/issues/82), [#146](https://github.com/simipoB/Galar-Native-Android/issues/146)) |
| Phones with Dimensity 6080 / 6300 | Dimensity 6080 / 6300 | Mali-G57 MC2 | ❌ Doesn't run yet | Closes a few frames in ([#138](https://github.com/simipoB/Galar-Native-Android/issues/138)) |
| Infinix and others with Helio G99 / G100 | Helio G99 / G100 | Mali-G57 MC2 | ❌ Doesn't run yet | Closes a few frames in with Arm's driver ([#89](https://github.com/simipoB/Galar-Native-Android/issues/89)); very slow with PanVK ([#153](https://github.com/simipoB/Galar-Native-Android/issues/153)) |
| Google Pixel 6a | Tensor G1 | Mali-G78 MP20 | 🟡 Plays with problems | Graphics glitches and flashing ([#150](https://github.com/simipoB/Galar-Native-Android/issues/150)) |

## PowerVR (some MediaTek Dimensity, Google Tensor G5)

| Device | Chip | GPU | Status | Notes |
|---|---|---|---|---|
| Google Pixel 10 Pro XL | Tensor G5 | PowerVR DXT-48-1536 | 🟡 Plays with problems | Runs, but well below 30 fps: the GPU is the limit ([#117](https://github.com/simipoB/Galar-Native-Android/issues/117)) |
| Phones with Dimensity 7060 | Dimensity 7060 | PowerVR BXM-8-256 | ❌ Doesn't run yet | Crashes ([#103](https://github.com/simipoB/Galar-Native-Android/issues/103)); use the phone's own driver, not Turnip or PanVK |

## Your device isn't listed?

Try it and tell us: open an [issue](https://github.com/simipoB/Galar-Native-Android/issues) with your phone's name, its chip and GPU, the app version, and whether it plays. If it doesn't, attach `PokemonShield-log-previous.txt` (or `PokemonSword-…`) from your Download folder. That's how this list grows.
