# Buddhas-Watch Firmware Build: Merge Strategy

## Objective
Consolidate 6 pull requests into a production-ready firmware with:
- ✅ App Store + Launcher
- ✅ Multi-protocol CSI streaming (BLE, Wi-Fi TCP/UDP, USB-C CDC)
- ✅ 7-module Settings App
- ✅ Built-in reference apps
- ✅ Compute node clients (Jetson, Steam Deck, Mobile, Laptop)

## Merge Order & Rationale

### Phase 1: Foundation (Firmware Core)
**PR #6 → PR #5 → PR #4**

These have **layered dependencies**:
- **PR #6**: WatchOS boot scaffold, app launcher, settings modules → **Builds on**
- **PR #5**: Hardware drivers, CSI handler, power management → **Builds on**
- **PR #4**: Watch apps, app store, companion apps

### Build Dependency Tree
```
PR #6 (WatchOS Scaffold)
  ↓
PR #5 (Hardware Drivers + Firmware Fixes)
  ↓
PR #4 (Watch Apps + Ecosystem)
  ↓
Ready for Compilation & Testing
```

## Merged Components

### From PR #6: WatchOS Scaffold
- Boot orchestration (`main.cpp`, `watch_os.cpp`)
- App launcher UI + touch interface
- Settings app (7 modules: Connectivity, Display, Audio, Power, Sensors, System, Security)
- Multi-protocol streaming manager
- Command receiver + persistence
- Build graph updates (PlatformIO, CMakeLists.txt)

### From PR #5: Hardware Drivers
- CSI handler (esp-csi integration)
- IMU driver (QMI8658 I2C)
- Audio driver (ES7210 I2S + ES8311 codec)
- Display driver (CO5300 AMOLED SPI)
- Touch driver (FT3168 I2C)
- Power management (AXP2101 via AXP192 library)
- microSD card (FatFS)
- BLE GATT service
- Wi-Fi control (raw 802.11 TX)
- Python: Quantum enhanced detection (Qiskit)
- Analysis tools: CSI analyzer, multi-node correlator

### From PR #4: Complete Ecosystem
- Watch apps: Settings, Wi-Fi CSI collector, BLE CSI broadcaster, App Manager
- App Store backend (Node.js/Express)
- Android companion app (BLE + Wi-Fi UDP)
- Desktop Python companion (tkinter GUI)
- Transport abstraction layer (UDP/BLE/USB unified)
- Unit tests for transport manager

## Conflict Resolution Strategy

If conflicts arise during merge:
1. **PR #6 takes priority** for boot/launcher/settings (most recent)
2. **PR #5 hardware drivers** are self-contained (no conflicts expected)
3. **PR #4 ecosystem** adds new directories (minimal conflicts)

## Post-Merge Verification

After merge to `main`:
```bash
# 1. Verify directory structure
ls firmware/
ls companion/
ls backend/
ls python/

# 2. Compile firmware
cd firmware
pio run -e buddhas_watch

# 3. Run Python tests
cd python
pytest tests/test_transport_manager.py -v

# 4. Verify apps structure
ls apps/
```

## Next Steps

1. ✅ Merge PR #6 (WatchOS core)
2. ✅ Merge PR #5 (Hardware drivers)
3. ✅ Merge PR #4 (Watch apps + ecosystem)
4. 🔄 Test compilation with PlatformIO
5. 🔄 Generate merged branch documentation
6. 🔄 Tag as `v1.0-wip` (work in progress)
7. 🔄 Create issue for remaining PRs (#1, #2, #3)
