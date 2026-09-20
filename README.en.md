# AntiHackerX

[中文](README.md) ｜ [**English**](README.en.md)

**Harden Java artifacts: bytecode obfuscation + class encryption + native compilation — three layers, each deeper than the last.**

> Run a decompiler on a conventionally obfuscated JAR and you get *unreadable code*.
> Run it on an AntiHackerX-packed JAR and you get **no code at all** — the classes are
> encrypted into a single payload, and the hot method bodies are machine code inside a `.so`.

A cross-platform (C++ / Qt6) Java protection tool with a GUI, built for **Minecraft
plugin and mod authors** and **commercial Java software vendors**. No installer and no
manual toolchain setup: on first launch it fetches the JDK and every build dependency it
needs.

## What it solves

Java `.class` files are decompilable by design. Ordinary obfuscators only scramble names —
the method bodies are still right there, perfectly readable in IDEA or JADX. To actually
stop reverse engineering, the code has to **not be in the JAR**.

AntiHackerX splits protection into three layers that can be combined freely:

| Layer | Technique | What the attacker faces |
|---|---|---|
| ① **Obfuscation** | Rename classes/packages/methods/fields/params, string encryption, integer XOR, junk code, hidden members | Meaningless names, diluted control flow |
| ② **Encryption** | Every class AES-256-GCM encrypted into a single payload, decrypted at runtime and `defineClass`-ed into the **host class loader** | No readable classes in the JAR |
| ③ **Native compilation** | Method bodies of selected classes transpiled to C++ and compiled into `.so` / `.dll` | Method bodies are machine code, not bytecode |
| ➕ **Hardening** | Anti-debug / anti-agent / tamper detection (ECDSA signature) / anti-VM / copyright notice | Dynamic analysis gets expensive too |

### Highlights

- 🔐 **Real class encryption (not just obfuscation)** — all classes are encrypted into
  `p.dat` (AES-256-GCM with the class name as additional authenticated data), then decrypted
  at runtime and defined via `MethodHandles.Lookup#defineClass` into the **server's own class
  loader**. No custom loader, so `plugin.yml`, reflection and
  `getClass().getClassLoader()` keep working normally.
- 🛡️ **Native compilation** — method bodies are moved into a native library through
  native-obfuscator, with **cross-compilation for both Linux and Windows**.
- 🧬 **Tamper detection** — every non-class entry in the artifact (`p.dat`, `*.so`, config
  files, `plugin.yml`) is covered by an ECDSA-P256 signature; any modification refuses to
  load. The public key lives in the native library, not in a Java constant.
- 🌐 **First-class Minecraft server and mod support** — detects `plugin.yml` automatically and works
  around the single-instance constraint of `JavaPlugin`. For Fabric mods all four
  `entrypoints` stages of `fabric.mod.json` are rewritten, so the payload is defined
  right inside `KnotClassLoader` (no loader replacement, mixin classes frozen plain).
- 🎨 **Qt6 GUI** — 13 obfuscation switches plus presets, JAR type detection, class scanning
  with per-class selection, live log and progress.
- 📦 **Zero setup** — the portable JDK, native-obfuscator, jar-obfuscator and the zig
  toolchain are all downloaded automatically.

## Supported artifact types

| Type | Detected by | Hardening support |
|---|---|---|
| Plain JAR | `Main-Class` in `MANIFEST.MF` | ✅ Full |
| Spring Boot fat JAR | `BOOT-INF/` | ✅ Full |
| Minecraft Paper / Bukkit plugin | `plugin.yml` | ✅ Full (dedicated entry template) |
| Fabric mod | `fabric.mod.json` | ✅ Full (all four `entrypoints` stages + Mixin frozen plain) |
| Forge mod | `mods.toml` / `mcmod.info` | ⚠️ Detected only |

## How it works

One packing run is an **11-step pipeline**. The important part is three steps:

```mermaid
flowchart LR
    A[Input JAR] --> B[1) jar-obfuscator<br/>bytecode obfuscation]
    B --> C[2) compile verifier module<br/>generate packing entry]
    C --> D[3) NOBF transpiles<br/>selected classes to C++]
    D --> E[4) zig cross-compiles<br/>Linux + Windows]
    E --> F[5) remaining classes AES-encrypted<br/>into p.dat]
    F --> G[6) sign + assemble]
    G --> H[Packed artifact]
```

### Three non-obvious design decisions

**① Why a "bridge class" is mandatory**

Bukkit/Paper's `JavaPlugin` constructor enforces a hard check: the instance must be created
by the server's own `PluginClassLoader`, and **one loader may only ever create one instance**
(the `plugin` field is `final`). So "let the entry extend `JavaPlugin` and then instantiate
the real plugin" is a dead end.

Instead the bridge class is inserted **between the real main class and `JavaPlugin`**; the
packer rewrites the real main class's superclass:

```
org.bukkit.plugin.java.JavaPlugin
        ↑
  bridge class (static init only: defines the payload)
        ↑
  real plugin main class (superclass repointed to the bridge)
```

Class initialization order then guarantees the payload is in place first, and the
`JavaPlugin` constructor runs exactly once.

**② Why "definer" classes are needed**

The decrypted classes must be defined by the **host class loader** (see above), and
`MethodHandles.Lookup#defineClass` requires the lookup to be in the **same package** as the
target class. So the packer emits one tiny definer class per package covered by the payload,
in the plaintext area; at runtime their `Lookup` is used to define the classes in that package.

**③ Why a set of classes must stay plaintext (and why it is small)**

Paper runs `Class.forName(main, true, this)` inside `PluginClassLoader.<init>` — that is,
*defineClass* (resolving direct superclasses and interfaces) and *bytecode verification*
(whose assignability checks really do load types) must complete **before** the payload can
exist. Therefore the following must stay plaintext:

- the main class itself plus its direct supertype chain, recursively;
- **every type the main class directly references**, plus the supertype closure of those;
- the decryptor itself (bridge class / runtime / definers).

Everything else is encrypted. Measured on GrimAC (19.5 MB, 4600 classes) the plaintext set
is only **62 classes** — 98.7% encrypted. Recursing one more level would balloon it to 4574
classes, i.e. no encryption at all, so the packer deliberately stops at one level.

## Project layout

```
AntiHackerX/
├── src/                        # C++ sources (Qt6 GUI + pipeline)
│   ├── main.cpp               # entry point (incl. bootstrap gate)
│   ├── mainwindow.h/cpp       # main window: options / class table / log
│   ├── packer_pipeline.h/cpp  # the 11-step packing pipeline (core)
│   ├── verify_module.h/cpp    # verifier module assembly: source release + entry generation
│   ├── jar_analyzer.h/cpp     # JAR type detection (5 types)
│   ├── class_scanner.h/cpp    # class scanning and selection
│   ├── obfuscator_controller.*# process-isolated invocation of the GPL component
│   ├── runtime_bootstrap.*    # bootstrap: auto-download JDK / deps / zig toolchain
│   ├── downloader.*           # HTTP downloader (progress, speed, ETA, redirects)
│   ├── download_dialog.*      # modal download window with progress bar
│   ├── pack_progress_dialog.* # packing progress window
│   ├── archive.*              # zip / tar.gz / tar.xz extraction
│   └── app_logger.*           # log files
├── AntiHackerXVerify/          # verifier module (MIT): guards + decrypt runtime + templates
│   ├── src/main/java/top/h3k4/ # AhxRuntime / AhxPayload / AhxSignature / guards
│   ├── templates/              # Bootstrap / PluginBootstrap entry templates
│   ├── tools/                  # AhxPacker (AES packing) / AhxSign (signing keys)
│   └── verify.qrc              # embedded resource manifest
├── jar-obfuscator/             # MIT, bytecode obfuscator fork (reference-remapping fixes)
├── native-obfuscator/          # GPL-3.0 component, separate git repository
├── docs/                       # documentation
├── build.sh                    # build script
├── install-deps.sh             # one-shot install of build deps (Qt6 / CMake / compiler)
├── test-jar-analyzer.sh        # self-test for JAR type detection
├── check-license-isolation.sh  # license isolation check (GPL component: process call only)
└── CMakeLists.txt              # CMake configuration
```

> `java/`, `libs/`, `build/`, `testp/` and `native-obfuscator/` are all excluded from the
> repository: the first three are downloads or build outputs, the last two are large test
> fixtures and a separately maintained GPL repository.

## Requirements

### Building the GUI

- **Qt6** (qt6-base-dev, including the Network module)
- **CMake** 3.16+
- **C++ compiler** (GCC 7+, Clang 6+, MSVC 2019+)

### Runtime dependencies

All four of the following are **downloaded automatically** when missing — you do not install
them by hand:

| Dependency | Purpose | Source |
|---|---|---|
| Portable JDK | Runs the obfuscators, compiles the verifier module | Adoptium |
| native-obfuscator | Transpiles method bodies to C++ | This project's fork releases |
| jar-obfuscator | Bytecode obfuscation | This project's fork |
| **zig** (bundles Clang) | Compiles the transpiled C++ | ziglang.org |

> Because zig ships with the tool, you do **not** need GCC / Clang / CMake installed
> system-wide, and you do **not** need Visual Studio. A single Linux machine produces both
> `x64-linux.so` **and** `x64-windows.dll`, so one artifact serves both platforms.

### Network

The tool needs access to GitHub and ziglang.org (dependency releases, JDK, toolchain).
Behind a proxy, set the `http_proxy` / `https_proxy` environment variables before launching.

## Quick start

### 1. Install build dependencies

#### Ubuntu / Debian
```bash
sudo apt update
sudo apt install qt6-base-dev cmake g++
```

#### Arch Linux
```bash
sudo pacman -S qt6-base cmake gcc
```

#### macOS
```bash
brew install qt@6 cmake
```

> No JDK needed — a portable one is downloaded on first launch if none is found.

### 2. Build the GUI

```bash
./build.sh
```

Or manually:

```bash
mkdir -p build && cd build
cmake ..
cmake --build . --config Release
```

### 3. Run it

```bash
./build/bin/AntiHackerX
```

On first launch dependencies are fetched with a progress bar:

1. Java is detected — if absent you are asked whether to download a portable JDK (~190 MB),
   after which `JAVA_HOME` is set automatically;
2. `native-obfuscator` and `jar-obfuscator` are fetched from this project's GitHub releases;
3. **the first time native compilation is actually needed**, a portable zig (~50 MB) is
   downloaded on demand.

To skip all of this, place `java/` and `libs/` next to the binary yourself.

## Usage

### Basic flow

1. Launch AntiHackerX (the first launch fetches the JDK and toolchain, with progress bars).
2. Pick the JAR to protect — the type is detected and hardenable classes are scanned.
3. Tick the obfuscation and protection options you want (the defaults are already a sound set).
4. Hit start and watch the log; the artifact is written to the output directory.

> Paper/Bukkit plugins additionally require the **path to the server API classes**
> (`org.bukkit.*`), otherwise the plugin entry of the verifier module cannot compile —
> the GUI reminds you.

### Obfuscation options (⭐ = on by default)

| Option | Description | Default |
|---|---|---|
| Class name obfuscation (with reference fixing) | Renames all classes and fixes references in the constant pool, generic signatures, annotations, `invokedynamic`, etc. | ⭐ on |
| Package name obfuscation (with reference fixing) | Renames package paths and moves classes. Breaks plugins referenced by reflection or resource paths | off |
| Method name obfuscation (with reference fixing) | Renames non-interface, non-overriding methods and fixes call sites | off |
| Field name obfuscation (with reference fixing) | Renames fields and fixes read/write sites. Careful with serialization / reflection | off |
| Method parameter name obfuscation | Affects debug info only (`LocalVariableTable` / `MethodParameters`) | off |
| Remove compile debug info | Drops `SourceFile` and line number tables | off |
| String AES encryption, decrypted at runtime | String constants are stored encrypted and decrypted by a generated class | ⭐ on |
| Strings via a global list | Strings never appear in the bytecode; fetched by index from a static list. Stronger, but larger bytecode | off |
| Integer constant multi-XOR | Splits `int` constants into several XOR operations | off |
| Junk code | Inserts never-executed decoy instructions, strength L1–L9 | ⭐ on (L2) |
| Hide methods from IDEA's decompiler | Writes special attributes so IDEA's decompiler cannot see them | ⭐ on |
| Hide fields from IDEA's decompiler | Same, for fields | ⭐ on |
| AI prompt injection | Injects a constant into every class — a notice addressed to AI/automated analysis systems asking them to refuse to explain the class. Only helps when code is pasted into a model; raises the cost, not a reliable protection. About +1.8 KB per class | off |

### Protection options

| Option | Effect |
|---|---|
| Anti-debug | Timer guard that detects the freeze caused by breakpoints |
| Anti-agent | Inspects `-javaagent` in the JVM launch arguments |
| **Tamper detection** | ECDSA-P256 signature over all non-class entries; refuses to load if modified |
| Anti-VM | Firmware / vendor fingerprint detection |
| Copyright notice | Shows a packing copyright notice at startup (falls back to logging on headless hosts) |
| Hide main class | Combined with "classes to hide", converts the selected classes to native code |

### What the artifact looks like

```
plugin.jar
├── <random.package>/          # random per artifact, so two packed plugins never collide
│   ├── <random>.class        # loader
│   └── x64-linux.so          # optionally x64-windows.dll (one artifact, both platforms)
├── top/h3k4/
│   ├── p.dat                 # all encrypted classes (AES-256-GCM)
│   └── p.sig                 # tamper-detection signature (ECDSA-P256)
├── <your.package>/*.class    # only the classes that must stay plaintext
├── plugin.yml               # untouched: `main` stays your original main class
└── META-INF/MANIFEST.MF
```

## Documentation

- [Project summary](docs/PROJECT_SUMMARY.md) — capabilities and key figures
- [Architecture](docs/ARCHITECTURE_REFACTOR.md) — module breakdown and evolution
- [Bootstrap](docs/RUNTIME_BOOTSTRAP.md) — JDK / dependency / toolchain self-provisioning
- [Deployment guide](docs/DEPLOYMENT_GUIDE.md) — packing and distribution
- [GUI development](docs/GUI_DEVELOPMENT.md) — layout and code organization
- [Obfuscator controller](docs/OBFUSCATOR_CONTROLLER.md) — process isolation and argument assembly
- [Loader analysis](docs/LOADER_ANALYSIS.md) — how the native loader works
- [kilij virtualization evaluation](docs/KILIJ_INTEGRATION.md) — compile-time VM virtualization data (⏸ deferred)
- [**Roadmap (TODO)**](TODO.md) — platform expansion (Fabric / Forge / plain JAR / Spring Boot) and capability plans
- [Release checklist](docs/RELEASE_CHECKLIST.md) — go through it before every release

*(The documents themselves are currently written in Chinese.)*

## Project status

Validated against **real commercial artifacts**: a 19.5 MB / 4600-class Paper anticheat
plugin (GrimAC) loads, enables and runs correctly after packing, and a small 36 KB plugin
is packed in about a second.

- ✅ 11-step pipeline working end to end
- ✅ Class encryption (AES-256-GCM + 3-shard key + per-package definers), main class and
  direct supertype chain kept plaintext
- ✅ Bridge class works around the single-instance constraint of `JavaPlugin`
- ✅ Per-artifact random naming (fixes the bootstrap class loader `LinkageError` that
  occurred when two packed plugins were installed together)
- ✅ Tamper detection: ECDSA-P256 signature over `p.dat` / `*.so` / `plugin.yml` and every
  other non-class entry
- ✅ 13 obfuscation options, presets and a class-scanning GUI
- ✅ Portable toolchain bootstrap + Linux/Windows cross-compilation

Outstanding work and known limitations live in [**TODO.md**](TODO.md) (the product roadmap).

## License

This project is licensed under the **GNU GPL v3.0** — see [`LICENSE`](LICENSE).

`native-obfuscator` is invoked as a **separate process**. Both are GPL-3.0, so process
isolation here is an **architectural choice, not a license requirement** — its value is that
AntiHackerX contains no upstream source at all, which leaves it free to change license
independently in the future.

> ℹ️ **Sample module exception**: the [`AntiHackerXVerify/`](AntiHackerXVerify/) subdirectory
> is licensed separately under **MIT** (see
> [`AntiHackerXVerify/LICENSE`](AntiHackerXVerify/LICENSE)). It is a demo program and a
> copyright-notice boilerplate you can embed in your own program — so you may copy it into a
> closed-source project without triggering GPL-3.0.

> ⚠️ **About the programs you pack with AntiHackerX**: the licensing of that code is governed
> by the **Output Exception** in native-obfuscator's upstream `LICENSE`, and is **unrelated to
> AntiHackerX's own GPL-3.0**. The exception explicitly permits linking, embedding, compiling
> and distributing the runtime code emitted by the tool under terms of your choosing. See
> <https://github.com/radioegor146/native-obfuscator/blob/master/LICENSE>.

## Contributing

Issues and pull requests are welcome.

- Looking for something to work on? [TODO.md](TODO.md) — P0 and P1 items live there.
- Before touching `native-obfuscator/`, read its `MODIFICATIONS.md` (that is a separate
  GPL-3.0 fork).
- Please make sure `./check-license-isolation.sh` still passes in your PR.

---

**Note**: native transpilation has a noticeable performance cost. Only apply it to your core
business logic.
