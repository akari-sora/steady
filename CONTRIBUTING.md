# Contributing to Steady

Contributions, bug reports, and power-efficiency improvements are welcome! The canonical repository and issue tracker are at <https://gitlab.com/aa22396584/steady>; the GitHub repository is a read-only mirror.

## License Agreement
By submitting a pull request or patch to this repository, you agree that your contributions will be licensed under the **GNU Affero General Public License v3.0 or later** (`AGPL-3.0-or-later`), matching the project [LICENSE](LICENSE).

## Development & Verification
Before opening a PR:
1. Build the desktop binary cleanly with zero warnings:
   ```bash
   ./bin/build
   ```
2. Run the ASan + UBSan test suite:
   ```bash
   ./bin/test
   ```
   On Windows, from an x64 MSVC prompt with Qt on `PATH` (or `QTDIR` set): `bin\build.bat` and `bin\test.bat`.
   The desktop code must build without warnings on Linux, macOS and Windows; see the README for the toolchains.
3. If modifying `android/`, run the plain-JVM logic tests, then rebuild and verify the APK signature:
   ```bash
   ./android/run-tests.sh
   ./android/build-apk.sh
   ```
   Decision logic belongs in classes without `android.*` imports so it can be tested this way.
4. Never commit personal IP addresses, TLS keys (`*.pem`), or Android signing keystores (`*.jks`).
