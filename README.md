# Smart Space Lighting System

This project is a smart lighting subsystem for a shared smart space prototype.  
The initial version focuses on building a minimal control pipeline between Raspberry Pi 4 and Pico W through UART and Linux device files.

## Features

- Terminal-based manual lighting control
- UART communication between Pico W and Raspberry Pi 4
- Linux device nodes for sensor and lighting control
- User-space daemon for monitoring sensor data
- Initial ACTIVE / IDLE / SLEEP state control logic

## Tech Stack

- C
- Linux Kernel Module
- Raspberry Pi 4
- Pico W
- UART
- Linux device file interface

---

# 智慧空間燈光系統

本專案為共享智慧空間原型中的智慧燈光子系統。  
初始版本著重於建立 Raspberry Pi 4 與 Pico W 之間的最小控制流程，透過 UART 與 Linux 裝置檔完成燈光控制與感測資料傳遞。

## 功能

- 終端機手動控制燈光
- Pico W 與 Raspberry Pi 4 之間的 UART 通訊
- 透過 Linux 裝置節點進行感測資料讀取與燈光控制
- 使用 user-space daemon 監控感測資料
- 初步 ACTIVE / IDLE / SLEEP 狀態控制邏輯

## 使用技術

- C
- Linux Kernel Module
- Raspberry Pi 4
- Pico W
- UART
- Linux 裝置檔介面