## Development Setup

### Requirements

- Windows x64 and [Git](https://git-scm.com/downloads).
- Stock **Unreal Engine 5.8** for plugin/host development; no custom engine is required.
- **Visual Studio 2022** with the C++ desktop/game development workloads, **MSVC v143 14.44** (Individual components), and a Windows SDK.
- **CMake 3.27+** on PATH for rebuilding UnrealGeometryEncoder.
- Internet access for the automatic download of the precompiled PRT SDK.

**Note** that the Plugin is called _Vitruvio_ internally.

1. Checkout the code via `git clone https://github.com/Esri/vitruvio.git`
2. For convenience the repository contains a project called _VitruvioHost_ with the actual Vitruvio Plugin in the _Plugins_ folder
3. Navigate to the _VitruvioHost_ folder in your checked out repository
4. Right click the _VitruvioHost.uproject_ file and run _Generate Visual Studio project files_. This will download the PRT library and setup the project. **Note** this might take a while
5. After the project files have been generated the project can be opened and compiled

To rebuild **UnrealGeometryEncoder**, follow the [standalone CMake instructions](../VitruvioHost/Plugins/Vitruvio/Extras/README.md). This build does not need Unreal: it downloads its own precompiled SDK using the same `PRT.version.json` as the plugin. PRT itself is never built from source.
