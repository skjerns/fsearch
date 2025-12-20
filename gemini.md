# Project Context: fsearch

**Note to future AI instances:** Please add any new, relevant information about the project, its structure, or user preferences to this file to maintain a persistent context.

## Project Overview

- **Project Name:** fsearch
- **Language:** C
- **Build System:** Meson
- **UI Framework:** GTK
- **Description:** Based on the file structure and content, this is a desktop application for Linux that provides fast file searching capabilities.

## Project Structure Outline

- **/src/**: Contains the core application source code, written in C (.c and .h files). This includes UI logic, database management, file utilities, and the main application entry point (`main.c`).
- **/data/**: Holds application resources such as the desktop entry (`.desktop.in.in`), icons (`.svg`), AppStream metadata (`.metainfo.xml.in`), and man pages.
- **/po/**: Manages internationalization and localization through translation files (`.po`).
- **/help/**: Contains user documentation in Mallard format (`.page` files).
- **meson.build**: The root configuration file for the Meson build system, which defines how the project is built.
- **.github/workflows/**: Contains CI/CD pipeline configurations for GitHub Actions, such as `build_test.yml`.
- **/tests/**: Contains unit tests for different modules of the application.
