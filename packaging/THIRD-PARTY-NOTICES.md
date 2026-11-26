# Third-party notices

Steady is licensed separately under AGPL-3.0-or-later. The release archives also contain software from the projects below. The adjacent files in this directory reproduce their licence terms; `BUNDLED-FILES.txt`, generated while packaging, lists the exact deployed files for that archive.

## Qt

The macOS and Windows archives bundle dynamically linked Qt 6 libraries, plugins and QML modules under the GNU Lesser General Public License version 3. The LGPL and incorporated GPL text are included as `Qt-LGPL-3.0-only.txt` and `Qt-GPL-3.0-only.txt`. Steady does not modify Qt. Users may replace the dynamically linked Qt libraries with compatible builds as permitted by the LGPL.

The macOS release is built from Qt 6.12.0 and the Windows release from Qt 6.10.3. Corresponding Qt source archives are available from:

- <https://download.qt.io/official_releases/qt/6.12/6.12.0/submodules/>
- <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/>
- <https://code.qt.io/cgit/>

The source version, qmake project files and build instructions for Steady are included in the public Steady source tree, providing the application code needed to rebuild and relink it with Qt.

## Platform inventory

The Linux archive bundles Project Nayuki's QR Code generator; Qt and LayerShellQt remain system libraries and are not copied into that archive.

The macOS archive bundles Qt plus the Homebrew libraries pulled into the app by `macdeployqt`: OpenSSL, ICU, GLib/GIO/GObject/GModule/GThread, gettext/libintl, D-Bus, FreeType, HarfBuzz, libpng, libjpeg-turbo, zstd, PCRE2, Brotli, md4c, Graphite2, double-conversion and libb2. Their project-specific terms are included here.

The Windows archive bundles Qt, the Qt OpenSSL TLS plugin, OpenSSL 3 (`openssl.exe`, `libssl` and `libcrypto`), and the Microsoft Visual C++ runtime selected by `windeployqt --compiler-runtime`. Depending on the Qt kit, `windeployqt` may also deploy Microsoft's D3D compiler, LLVM's DX compiler and Mesa's software OpenGL renderer; the LLVM and Mesa terms are included here. Microsoft redistributable components remain governed by the Microsoft Visual Studio and Windows SDK redistribution terms: <https://visualstudio.microsoft.com/license-terms/>.

Qt itself incorporates or links zlib, PCRE2, libpng, FreeType, HarfBuzz and double-conversion on supported platforms. Their terms are included whether they appear as separate files or inside a Qt binary.

## Other components

- Project Nayuki QR Code generator 1.8.0 — MIT; `qrcodegen-MIT.txt`.
- OpenSSL — Apache-2.0 and OpenSSL notices; `OpenSSL-LICENSE.txt`.
- ICU — Unicode/ICU terms; `ICU-LICENSE.txt`.
- GLib and gettext — LGPL/GPL terms in the named files.
- D-Bus, FreeType, HarfBuzz, libpng, libjpeg-turbo, zstd, PCRE2, Brotli, md4c, Graphite2, double-conversion, libb2 and zlib — project terms in the correspondingly named files.
- LLVM/DXC and Mesa software OpenGL — `LLVM-LICENSE.txt` and `Mesa-LICENSE.rst`.
