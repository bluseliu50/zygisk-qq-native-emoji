#!/bin/bash
set -e

# === Configuration ===
VERSION="v1.0.0"
VERSION_CODE=1
MODULE_ID="qq_native_emoji"
ABIS=("arm64-v8a" "armeabi-v7a")

NDK_PATH="${ANDROID_NDK_HOME:?Environment variable ANDROID_NDK_HOME is not set}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

echo "=== Building native libraries ==="
for ABI in "${ABIS[@]}"; do
    echo "--- Building ${ABI} ---"
    ABI_BUILD_DIR="${BUILD_DIR}/cmake/${ABI}"
    rm -rf "${ABI_BUILD_DIR}"
    mkdir -p "${ABI_BUILD_DIR}"
    cd "${ABI_BUILD_DIR}"

    "${CMAKE_BIN}" "${ROOT_DIR}/native" \
        -DCMAKE_TOOLCHAIN_FILE="${NDK_PATH}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_static

    "${CMAKE_BIN}" --build . --target qqemoji -- -j"$(nproc)"
    cd "${ROOT_DIR}"
done

echo "=== Packaging module ==="
MODULE_DIR="${BUILD_DIR}/module"
rm -rf "${MODULE_DIR}"
mkdir -p "${MODULE_DIR}/zygisk"

cp "${ROOT_DIR}/module/module.prop" "${MODULE_DIR}/"
cp "${ROOT_DIR}/module/customize.sh" "${MODULE_DIR}/"
cp "${BUILD_DIR}/cmake/arm64-v8a/libqqemoji.so" "${MODULE_DIR}/zygisk/arm64-v8a.so"
cp "${BUILD_DIR}/cmake/armeabi-v7a/libqqemoji.so" "${MODULE_DIR}/zygisk/armeabi-v7a.so"

# Inject version into module.prop
sed -i "s/^version=.*/version=${VERSION}/" "${MODULE_DIR}/module.prop"
sed -i "s/^versionCode=.*/versionCode=${VERSION_CODE}/" "${MODULE_DIR}/module.prop"

ZIP_NAME="${MODULE_ID}-${VERSION}.zip"
cd "${MODULE_DIR}"
rm -f "${BUILD_DIR}/${ZIP_NAME}"
zip -r "${BUILD_DIR}/${ZIP_NAME}" .
cd "${ROOT_DIR}"

echo "=== Done: ${ZIP_NAME} ==="
ls -lh "${BUILD_DIR}/${ZIP_NAME}"
