# LVGL + SquareLine ESP32 template

This project is a **teaching template** for students to:

- design a UI in **SquareLine Studio**
- export it as C code
- build and **flash it directly to an ESP32** (e.g. ByteLab DevKit)

The LVGL and driver setup is already done – students only need to work in SquareLine and (optionally) in `ui_events.c`.

- **ESP-IDF**: 5.0.x  
- **LVGL**: 8.3.4  
- **SquareLine Studio**: 1.5.4  

---

### 1. Prerequisites

- Install ESP-IDF and make sure `idf.py` works.
- Install SquareLine Studio **1.5.4**. (Free trial)
- Clone/download this project.
- This template uses **git submodules**:
  - `git submodule update --init --recursive`
  - Apply the driver patch with the provided scripts (recommended):
    - **Windows (PowerShell)**: `.\scripts\apply_patch.ps1`
    - **Linux/macOS (bash)**: `./scripts/apply_patch.sh`

These scripts apply `patches/lvgl_esp32_drivers_8-3.patch` so the display drivers match LVGL 8.3.4.

If you use the Byte Lab DevKit, copy defaults once:

- Linux/macOS: `cp sdkconfig.defaults sdkconfig`  
- PowerShell: `Copy-Item sdkconfig.defaults sdkconfig`

---

## 2. Configure or import a SquareLine project

You can:

- **Use the existing example project** in `components/ui_app/squareline/project`, or  
- **Create your own project** and point it into this folder.

For a new SquareLine project:

1. Create a new project.  
2. Platform: **Espressif**.  
3. Board: **ESP WROVER KIT**.  
4. Resolution: **240x320 or 320x240**, rotation 90°   
5. Color depth: **16 bit swap**.  
6. LVGL version: **8.3.4**.

Then in **File → Project Settings → FILE EXPORT**:

- **Project Export Root** → `.../BL_DevKit_template/components/ui_app/squareline/project/`  
- **UI Files Export Path** → `.../BL_DevKit_template/components/ui_app/squareline/`  
- Call functions export file: `.c`

Whenever you **Export UI files** in SquareLine, the generated C files will go directly into this ESP-IDF project.

### Import the existing `.spj` into SquareLine

If you want to start from the provided demo UI instead of creating a new one:

1. Open **SquareLine Studio**.  
2. Choose **Open project** (or `File → Open project`).  
3. Navigate to  
   `components/ui_app/squareline/project/`  
4. Select the `.spj` file (for example `esp32_gui.spj`) and open it.  
5. Check **Project Settings → FILE EXPORT** and verify that:
   - **Project Export Root** is `components/ui_app/squareline/project/`  
   - **UI Files Export Path** is `components/ui_app/squareline/`

After that, you can modify the UI, then use **Export UI files** to update the C code used by this ESP-IDF project.

---

## 3. Build and flash your UI to ESP32

1. Open a terminal in the project root.  
2. Source ESP-IDF environment (e.g. `. $HOME/esp/esp-idf/export.sh` or the ESP-IDF PowerShell script).  
3. Make sure the **SquareLine app** is selected (it is the default):
   - `idf.py menuconfig` → `Component config → UI application` → **SquareLine generated example**.
4. Build:
   - `idf.py build`
5. Flash (adjust port as needed):
   - Windows: `idf.py -p COMx flash`  
   - Linux/macOS: `idf.py -p /dev/ttyUSB0 flash`
6. Monitor:
   - Windows: `idf.py -p COMx monitor`  
   - Linux/macOS: `idf.py -p /dev/ttyUSB0 monitor`

If you are using the Byte Lab DevKit, set the **TCH_IRQ switch** on the peripheral module to **OFF**.

### Project structure and submodules

This template was built from an LVGL demo project with support for several display and touch controllers.

- `components/ui_app/squareline/` – exported C UI files (`ui.c`, `ui.h`, helpers, images, screens, etc.).  
- `components/ui_app/squareline/project/` – SquareLine project files (`.spj`, `.sll`, …).  
- `components/ui_app/ui_app.c` – calls `ui_init()` from the SquareLine-generated code.  
- `components/ui_app/squareline/ui_events.c/.h` – where you add your own logic; these files survive re-exports from SquareLine.

You normally don’t need to touch any LVGL or driver code – focus on SquareLine and `ui_events.c`.

This template depends on

- [lvgl](https://github.com/lvgl/lvgl)
- [lvgl_esp32_drivers](https://github.com/lvgl/lvgl_esp32_drivers) (patched for LVGL 8.3 – see `patches/lvgl_esp32_drivers_8-3.patch`)

---

## 4. FAQ / common issues

### SquareLine export error on Windows (weird `NullReference` or `AAAA`-style messages)

Sometimes SquareLine fails export on Windows with confusing internal errors. One common cause is **system date/time being wrong**.

- Make sure **Windows date, time and timezone are correct** and that automatic time sync is enabled.  
- Close SquareLine, fix the system time, then reopen the project and try **Export UI files** again.

If it still fails, also double-check:

- `File → Project Settings → FILE EXPORT` paths (see section 2).  
- `File → Project Settings → Assets` has all assets inside `components/ui_app/squareline/project/assets/` and none pointing outside the project tree.

### Linux: `idf.py` can’t open `/dev/ttyUSB0`

If `idf.py flash` or `idf.py monitor` says it can’t open `/dev/ttyUSB0` (permission denied or device not found):

- Check that the board is connected and the device exists:
  - `ls /dev/ttyUSB*`
- If it exists but you get **permission denied**, add your user to the `dialout` (or equivalent) group and re-login:

```bash
sudo usermod -a -G dialout $USER
```

- On some distros the group might be `uucp` or `plugdev` instead of `dialout`.  
- After changing groups, **log out and back in**, or reboot, then retry `idf.py -p /dev/ttyUSB0 flash`.