# redmatch2-offsets

A static IL2CPP offset parser for **Redmatch 2**. Takes the output of [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) and generates field offsets and method RVAs in multiple formats — no injection, no runtime, just static analysis.

Output formats: `.hpp` · `.json` · `.txt` · `.cs` · `.rs`

---

## Requirements

- Windows x64
- [Il2CppDumper v6.7.x](https://github.com/Perfare/Il2CppDumper/releases) — to produce `dump.cs`
- A C++23 compiler to build `parse.cpp`:
  - **MSVC** — Visual Studio 2022 or later
  - **MinGW/GCC** — g++ 13+
  - **Clang** — clang++ 17+

---

## Step 1 — Locate the game files

Find your Redmatch 2 installation folder. You need two files:

| File | Path |
|------|------|
| `GameAssembly.dll` | `Redmatch 2/GameAssembly.dll` |
| `global-metadata.dat` | `Redmatch 2/Redmatch 2_Data/il2cpp_data/Metadata/global-metadata.dat` |

---

## Step 2 — Run Il2CppDumper

1. Download the latest `Il2CppDumper-win-v6.7.x.zip` from the [releases page](https://github.com/Perfare/Il2CppDumper/releases)
2. Extract and launch `Il2CppDumper.exe`
3. When prompted, select:
   - **Binary:** `GameAssembly.dll`
   - **Metadata:** `global-metadata.dat`
4. Choose output mode **Auto** and pick an output folder
5. Wait for it to finish — you will get several files, but only `dump.cs` is needed

```
il2cpp_output/
├── dump.cs           ← this one
├── script.json
├── stringliteral.json
├── il2cpp.h
└── DummyDll/
```

---

## Step 3 — Build the parser

### MSVC (Visual Studio)

Open a **Developer Command Prompt** and run:

```bat
cl /std:c++latest /O2 /EHsc parse.cpp /Fe:parse.exe
```

### MinGW / GCC

```bash
g++ -std=c++23 -O2 -o parse.exe parse.cpp
```

### Clang

```bash
clang++ -std=c++23 -O2 -o parse.exe parse.cpp
```

---

## Step 4 — Run the parser

```bat
parse.exe --input "C:\path\to\il2cpp_output" --output "C:\path\to\offsets"
```

If `parse.exe` is in the same folder as `dump.cs`, just run:

```bat
.\parse.exe --output C:\offsets
```

> **PowerShell note:** PowerShell requires `.\` before executables in the current directory.
> Use `.\parse.exe` instead of `parse.exe`.

### Options

| Flag | Short | Description | Default |
|------|-------|-------------|---------|
| `--input` | `-i` | Folder containing `dump.cs` | `.` (current dir) |
| `--output` | `-o` | Folder to write output files | `./offsets` |
| `--help` | `-h` | Show usage | — |

---

## Output

The parser creates the following files in your output folder:

```
offsets/
├── rm2_offsets.hpp     ← C++ header with constexpr offsets
├── rm2_offsets.json    ← structured JSON dump
├── rm2_dump.txt        ← human-readable plaintext dump
├── Rm2Offsets.cs       ← C# constants
└── rm2_offsets.rs      ← Rust constants
```

### Example — `rm2_offsets.hpp`

```cpp
namespace HealthSyncer {
    // parent: SyncedBehaviour
    // Fields
    constexpr std::ptrdiff_t maxHealth      = 0x20; // int
    constexpr std::ptrdiff_t health         = 0x24; // int
    constexpr std::ptrdiff_t healthDisplays = 0x28; // ValueDisplay[]
    constexpr std::ptrdiff_t OnDamaged      = 0x30; // Activator[]
    constexpr std::ptrdiff_t OnHealed       = 0x38; // Activator[]
    constexpr std::ptrdiff_t OnDied         = 0x40; // Activator[]
    constexpr std::ptrdiff_t obf_3a8fc120   = 0x48; // MyceliumIdentity [obf: ↈ...ↈ]
    // Methods
    // [RVA 0x020d8be0] ↈ...ↈ() -> void
}
```

### Example — `rm2_offsets.json`

```json
{
  "classes": [
    {
      "name": "HealthSyncer",
      "parent": "SyncedBehaviour",
      "fields": [
        { "name": "health", "type": "int", "offset": 36, "offset_hex": "0x24", "static": false }
      ]
    }
  ]
}
```

### Example — `rm2_dump.txt`

```
Class: HealthSyncer : SyncedBehaviour
  [0x0020] maxHealth : int
  [0x0024] health : int
  [0x0028] healthDisplays : ValueDisplay[]
```

---

## How it works

Redmatch 2 is built with Unity's **IL2CPP** backend, which compiles C# game logic into a native `GameAssembly.dll`. The original C# metadata — class names, field names, offsets — is stored separately in `global-metadata.dat`. Il2CppDumper reads both files and reconstructs a C#-like `dump.cs` that annotates every field with its memory offset.

`parse.cpp` reads `dump.cs` line by line using a state machine:

1. Detects `// Namespace:` comments to track the current namespace
2. Detects `class` declarations to open a new class entry
3. Matches field lines of the form `public int health; // 0x24` — extracting the type, name, offset, and whether the field is static — all in a single pass, no multi-line lookahead needed
4. Matches `// RVA: 0x... Offset: 0x...` comments followed by method declarations to record method addresses
5. Handles obfuscated Unicode identifiers (`ↈ...ↈ`) without crashing — obfuscated field names are hashed to a stable `obf_XXXXXXXX` identifier and the original symbol is preserved in the comment

The result is written out in all five formats simultaneously.

---

## Notes

**Obfuscated names** — Redmatch 2 obfuscates most method and some field names using Unicode symbols (`ↈ...ↈ`). The parser handles this gracefully:
- Obfuscated field names are renamed to `obf_XXXXXXXX` (hash-based, stable across runs on the same binary)
- The original obfuscated name is preserved in the inline comment
- Obfuscated methods are listed in comments as-is with their RVA

**Inherited fields** — Il2CppDumper includes fields from base classes in the child class layout. This is intentional — it gives you the full memory layout of the object without having to trace the inheritance chain manually.

**Static fields** — Static fields are marked with the `_static` suffix in `.hpp` and `.cs` output. Their offset is relative to the static field data region, not the object instance.

---

## Related

- [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) — the tool that produces `dump.cs` from `GameAssembly.dll`
- [Streets of Rogue Offset Dumper](https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper) — the Mono runtime version of this tool (for non-IL2CPP Unity games)
