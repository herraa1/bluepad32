# Bluepad32 Architecture

WIP

```
┌──────────────┐  ┌──────────┐  ┌─────────┐  ┌──────────────┐
│              │  │  NINA /  │  │         │  │              │
│ Unijoysticle │  │  AirLift │  │ Arduino │  │  MightyMiggy │     Platforms
│              │  │          │  │         │  │              │
└────────┬─────┘  └────┬─────┘  └────┬────┘  └───────┬──────┘
         │             │             │               │
         │             │             │               │
         │             │             │               │
         │     ┌───────▼─────────────▼──────┐        │
         └─────►                            ◄────────┘
               │                            │
               │                            │
               │         Bluepad32          │                      Firmware
               │                            │
               │                            │
               │                            │
               └────┬──────────────┬────────┘
                    │              │
                    │              │
                    │     ┌────────▼────────┐
                    │     │                 │
                    │     │     BTstack     │                      Bluetooth Stack
                    │     │                 │
                    │     └────────┬────────┘
                    │              │
              ┌─────▼──────────────▼────────┐
              │                             │
              │          ESP-IDF            │                      ESP APIs
              │                             │
              └──────────────┬──────────────┘
                             │
                             │
              ┌──────────────▼──────────────┐
              │                             │
              │    ESP32 Microcontroller    │                      Hardware
              │                             │
              └─────────────────────────────┘
```