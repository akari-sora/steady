#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$ROOT_DIR/.." && pwd)"
OUT_DIR="$PROJECT_DIR/build"
WORK_DIR="$ROOT_DIR/build"

# Resolve JDK 21+ automatically if JAVA_HOME is not explicitly set
if [[ -z "${JAVA_HOME:-}" ]]; then
  if [[ -d "$HOME/.local/share/mise/installs/java/zulu-latest" ]]; then
    JAVA_HOME="$HOME/.local/share/mise/installs/java/zulu-latest"
  else
    JAVA_HOME="$(find "$HOME/.local/share/mise/installs/java" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -n 1 || true)"
  fi
fi

ANDROID_SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
BUILD_TOOLS="$(find "$ANDROID_SDK/build-tools" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -n 1)"
ANDROID_JAR="$(find "$ANDROID_SDK/platforms" -maxdepth 2 -name android.jar | sort -V | tail -n 1)"

export PATH="$JAVA_HOME/bin:$BUILD_TOOLS:$PATH"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/res_compiled" "$WORK_DIR/gen" "$WORK_DIR/obj" "$WORK_DIR/dex" "$OUT_DIR"

# 1. Compile Android resources
aapt2 compile --dir "$ROOT_DIR/res" -o "$WORK_DIR/res_compiled/res.zip"

# 2. Link resources & generate R.java + base APK
aapt2 link \
  -o "$WORK_DIR/unaligned.apk" \
  -I "$ANDROID_JAR" \
  --manifest "$ROOT_DIR/AndroidManifest.xml" \
  --java "$WORK_DIR/gen" \
  --auto-add-overlay \
  "$WORK_DIR/res_compiled/res.zip"

# 3. Compile Java sources targeting Java 11 bytecode for Android D8
find "$ROOT_DIR/src" "$WORK_DIR/gen" -name '*.java' > "$WORK_DIR/sources.txt"
javac \
  --release 11 \
  -classpath "$ANDROID_JAR" \
  -d "$WORK_DIR/obj" \
  @"$WORK_DIR/sources.txt"

# 4. Dex bytecode with d8
mapfile -t CLASS_FILES < <(find "$WORK_DIR/obj" -name '*.class')
d8 \
  --min-api 26 \
  --lib "$ANDROID_JAR" \
  --output "$WORK_DIR/dex" \
  "${CLASS_FILES[@]}"

# 5. Add classes.dex into APK
(cd "$WORK_DIR/dex" && zip -q -u "$WORK_DIR/unaligned.apk" classes.dex)

# 6. Align APK
zipalign -f 4 "$WORK_DIR/unaligned.apk" "$WORK_DIR/aligned.apk"

# 7. Persistent local keystore in ~/.local/share/steady/ (migrating legacy ~/.cache key if present)
# For external release distribution, always override STEADY_KEYSTORE and STEADY_KS_PASS with your own secret.
KEY_DIR="$HOME/.local/share/steady"
KEYSTORE="${STEADY_KEYSTORE:-$KEY_DIR/android-signing.jks}"
LEGACY_KEYSTORE="$HOME/.cache/steady/android-signing.jks"
mkdir -p "$(dirname "$KEYSTORE")"
chmod 700 "$(dirname "$KEYSTORE")"

if [[ "$KEYSTORE" == "$KEY_DIR/android-signing.jks" && ! -f "$KEYSTORE" && -f "$LEGACY_KEYSTORE" ]]; then
  mv "$LEGACY_KEYSTORE" "$KEYSTORE"
  chmod 600 "$KEYSTORE"
fi

export STEADY_KS_PASS="${STEADY_KS_PASS:-steady1234}"
if [[ ! -f "$KEYSTORE" ]]; then
  keytool -genkeypair -v \
    -keystore "$KEYSTORE" \
    -storepass:env STEADY_KS_PASS \
    -keypass:env STEADY_KS_PASS \
    -alias steady \
    -keyalg RSA \
    -keysize 2048 \
    -validity 3650 \
    -dname "CN=Steady Sender, O=Omarchy, C=TW" >/dev/null 2>&1
  chmod 600 "$KEYSTORE"
fi

APK_OUT="$OUT_DIR/steady-sender.apk"
apksigner sign \
  --ks "$KEYSTORE" \
  --ks-pass env:STEADY_KS_PASS \
  --key-pass env:STEADY_KS_PASS \
  --ks-key-alias steady \
  --out "$APK_OUT" \
  "$WORK_DIR/aligned.apk"

apksigner verify --verbose "$APK_OUT" | head -n 6
ls -lh "$APK_OUT"
