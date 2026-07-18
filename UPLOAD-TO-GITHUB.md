# Upload to GitHub

1. Extract the GitHub-ready source ZIP.
2. Replace the repository contents with the extracted files.
3. Make sure `.github/workflows/build-windows.yml`, `installer/MultichannelBridge.iss`, and `VERSION` are present.
4. Commit to `main` or `master`.
5. Open **Actions → Build Multichannel Bridge for DistroAV (Windows)**.
6. Run the workflow and wait for a green result.
7. Download the Windows x64 artifact.

The workflow checks out DistroAV 6.2.1, applies the patch, builds the DLL, creates a portable ZIP, downloads the official Inno Setup compiler, creates the setup EXE, and generates SHA-256 checksums.
