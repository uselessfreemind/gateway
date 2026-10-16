# Omron SCADA — C++ / WinUI 3 branch

This branch (`feature/cpp-winui3-scada`) adds a **native C++20 SCADA engine and WinUI 3 desktop client** in [`native/`](native/). The original C# implementation remains available in its existing directories and on `main`; see [its documentation](docs/csharp-backend.md).

The native engine implements asynchronous read-only FINS/UDP and EtherNet/IP explicit messaging for up to 500 controllers, a configurable **10 ms scan target**, archive queries and automatic one-calendar-month retention, and an equipment-side SECS-II/HSMS subset. The WinUI 3 source includes live values, controller/scan diagnostics and an archive-backed trend chart.

**Current limits:** the full 500 × 700-tag workload did not sustain 10 ms scans with the SQLite archive. WinUI 3 cannot be built or launched in the Linux development environment and is unverified until the Windows build runs. The documented SECS/GEM subset is not a full SEMI E30 implementation. See [native build instructions, architecture and commissioning requirements](native/README.md) and [measured validation](native/VALIDATION.md).

## Native Linux backend

Requires CMake 3.24+, a C++20 compiler, make, Tcl and Python 3. Dependencies are downloaded from pinned official source archives and checked by SHA-256.

```bash
bash native/scripts/build-linux.sh
./native/build/scada_gateway --demo --controllers 2 --tags 20 --interval 10 --run-for 5
```

## Windows desktop

Use Windows 10 2004+ / Windows 11 with Visual Studio 2022 C++/WinUI workloads, a Windows SDK, CMake, Git, NuGet and Python 3. From PowerShell:

```powershell
./native/scripts/build-windows.ps1 -Configuration Release
./native/winui/out/x64/Release/Scada.Desktop.exe
```

The script builds/tests the native engine, restores pinned WinUI dependencies and builds the desktop project. **Run demo** starts simulated data. **Start site** opens the JSON file entered in the path field; configure the real PLC addresses and tag types first using `native/config/site.example.json`. Nothing contacts the sample factory addresses automatically.

The Windows CI workflow is included at [`.github/workflows/native.yml`](.github/workflows/native.yml). A written workflow is not evidence of a successful Windows build or GUI test.
