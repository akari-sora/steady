# Contributing to Steady

Contributions, bug reports, and power-efficiency improvements are welcome!

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
3. If modifying `android/`, rebuild and verify the APK signature:
   ```bash
   ./android/build-apk.sh
   ```
4. Never commit personal IP addresses, TLS keys (`*.pem`), or Android signing keystores (`*.jks`).
