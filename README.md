# D3D12 PBR Engine

A Physically Based Rendering (PBR) engine built from scratch using DirectX 12. This project implements Image-Based Lighting (IBL), Spherical Harmonics (SH) for irradiance, and a complete Cook-Torrance BRDF pipeline.

## 🌟 Features
* **DirectX 12 API**: Modern graphics pipeline with explicit resource management and synchronization.
* **Physically Based Rendering**: Cook-Torrance BRDF (Albedo, Normal, Metallic, Roughness, AO).
* **Image-Based Lighting (IBL)**: Real-time Spherical Harmonics (SH) irradiance calculation and specular prefiltering via Compute Shaders.
* **GLTF/GLB Support**: Robust 3D model loading using Assimp.
* **Triple Buffering**: Smooth frame delivery using DXGI SwapChain flip-discard model.

---

## 🛠️ Prerequisites

To build and run this project, ensure your environment meets the following requirements:
* **OS:** Windows 11 (or Windows 10)
* **IDE:** Visual Studio 2022
  * *Required Workload:* "Desktop development with C++"
  * *Required Component:* Windows 10/11 SDK (e.g., 10.0.19041.0 or newer)

---

## 📦 Dependency Setup (Crucial Steps for Beginners)

This project relies on external libraries managed through both **vcpkg** and **NuGet**. **Please follow these steps carefully to configure your environment.**

### 1. Install `Assimp` via vcpkg (Step-by-Step)
We use `vcpkg` to manage the Assimp library for model loading. If you don't have vcpkg installed, open your terminal (PowerShell or Git Bash) and run the following commands:

**Step 1.1: Clone the vcpkg repository**
```bash
git clone [https://github.com/microsoft/vcpkg.git](https://github.com/microsoft/vcpkg.git)
cd vcpkg
```

**Step 1.2: Run the bootstrap script**
```bash
.\bootstrap-vcpkg.bat
```

**Step 1.3: Integrate vcpkg with Visual Studio**
*This is a critical step! It allows Visual Studio 2022 to automatically find the libraries installed by vcpkg without manual path configuration.*
```bash
.\vcpkg integrate install
```

**Step 1.4: Install Assimp (x64-windows)**
*Make sure to specify the 64-bit Windows architecture.*
```bash
.\vcpkg install assimp:x64-windows
```
*Wait for the compilation to finish. Once done, Visual Studio will automatically recognize `#include <assimp/...>`.*

### 2. Install DirectX Helpers via NuGet
The project uses the DirectX Tool Kit and Agility SDK. These are managed via Visual Studio's built-in NuGet Package Manager.

1. Open `LearnDirectX.sln` in Visual Studio 2022.
2. Right-click on your project in the **Solution Explorer** and select **Manage NuGet Packages...**.
3. Go to the **Browse** tab and install the following packages:
   * `directxtk12_desktop_win10` (by Microsoft)
   * `Microsoft.Direct3D.D3D12` (DirectX 12 Agility SDK by Microsoft)

---

## ⚠️ Important Configuration: HLSL Shader Properties

By default, Visual Studio tries to compile `.hlsl` files during the build process, which will cause the build to fail because this engine compiles shaders dynamically at **runtime**.

**You MUST disable build-time compilation for all shaders:**
1. In the **Solution Explorer**, open the `Shaders` folder.
2. Select **ALL** `.hlsl` files.
3. Right-click the selected files and choose **Properties**.
4. In the Properties window, change the **Item Type** (under Configuration Properties -> General) to **Does not participate in build** (or *Document*).
5. Click **Apply** and **OK**.

*(Note: The project folders `Models`, `Shaders`, and `HDRs` are already structured in this repository. Just ensure your working directory in VS is set correctly so the engine can find them.)*

---

## 🚀 Build and Run

1. Ensure your Visual Studio solution platform is set to **x64**.
2. Set the configuration to **Debug** or **Release**.
3. Press **F5** to build and launch the engine.

## 🎮 Controls

* **W / A / S / D**: Move the camera (Forward, Left, Backward, Right)
* **Hold Right Mouse Button + Drag**: Look around (Rotate camera view)
* **Mouse Scroll Wheel**: Zoom in / out
* **ESC**: Exit the application

---
*Created with C++ and DirectX 12.*
