# playcard-system

Cashless card system for a kids' play center: RFID cards hold points, an ESP32
reader on each arcade machine deducts points and sends a coin pulse, and a PC
backend will keep the accounts (not built yet).

```
firmware/  ESP32 arcade reader (PlatformIO + Wokwi simulation)
backend/   PC server (later)
web/       cashier / admin web UI (later)
docs/      notes
```

## Run the simulation

Needs VS Code with the **PlatformIO IDE** and **Wokwi Simulator** extensions
(Wokwi asks for a free license the first time).

1. Open the `firmware/` folder in VS Code.
2. Build: `pio run` (or the PlatformIO ✓ button).
3. `F1` → **Wokwi: Start Simulator**.
4. Click the RC522 board → pick a card → **Tap**.

Preset cards (offline table in `firmware/src/main.cpp`):

| UID         | Card   | Points | Note    |
|-------------|--------|--------|---------|
| 01:02:03:04 | Blue   | 100    |         |
| 11:22:33:44 | Green  | 10     |         |
| 55:66:77:88 | Yellow | 0      |         |
| AA:BB:CC:DD | Red    | 50     | blocked |

A good tap shows `OK -10`, lights the orange "PC817 LED" for 100 ms, the relay
pulls GPIO4 low, and the green "game running" LED stays on for 10 s.
