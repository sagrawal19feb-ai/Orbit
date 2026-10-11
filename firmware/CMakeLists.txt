# 🛰️ Orbit — Gemini voice assistant firmware for ESP32-S3
#
# Components we don't use (mqtt, bt, openthread, protobuf-c, protocomm,
# esp_local_ctrl, wifi_provisioning, cmock, idf_test, esp_hid, ieee802154) are
# physically moved out of the IDF tree by tools/ensure_idf.sh — several of them
# need git submodules we deliberately don't fetch. WiFi (esp_wifi + wpa_supplicant
# + esp_phy + esp_coex) and spiffs (required by esp-sr) stay in the tree.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orbit)
