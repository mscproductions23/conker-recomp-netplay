#!/bin/sh
set -eu

cd "$(dirname "$0")/../.." || exit 1

fail() {
  printf '%s\n' "package_linux: $*" >&2
  exit 1
}

[ ! -f host/build/ConkerRecomp ] && fail "ConkerRecomp binary not found"
[ ! -d host/build/assets ] && fail "assets directory not found"

out_dir="${1:-package}"
tag="${2:-dev}"
STAGE="$out_dir/ConkerRecompiled"
echo "==> Preparing stage directory in $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib"

echo "==> Copying binary and assets"
cp host/build/ConkerRecomp "$STAGE/ConkerRecomp.bin"
strip "$STAGE/ConkerRecomp.bin" || echo "==> Warning: Failed to strip binary"
cp -r host/build/assets "$STAGE/assets"

echo "==> Bundling required shared libraries"
ldd_output=$(ldd "$STAGE/ConkerRecomp.bin")
libraries=$(echo "$ldd_output" | awk '
  {
    n=$1;
    if (n ~ /^libprotobuf\.so/ || n ~ /^libcrypto\.so/) print
  }'
)
if echo "$libraries" | grep -q "not found"; then
  fail "One or more required libraries not found"
fi

if ! printf '%s\n' "$libraries" | awk '$1 ~ /^libprotobuf\.so/ {f=1} END {exit !f}'; then
  fail "libprotobuf.so not found in ldd output"
fi

if ! printf '%s\n' "$libraries" | awk '$1 ~ /^libcrypto\.so/ {f=1} END {exit !f}'; then
  fail "libcrypto.so not found in ldd output"
fi

while read -r line; do
  lib_path=$(echo "$line" | awk -F' => ' '{print $2}' | awk -F' ' '{print $1}')
  [ -z "$lib_path" ] || [ ! -f "$lib_path" ] && fail "Failed to resolve library path: $lib_path"
  cp -L "$lib_path" "$STAGE/lib/"
done <<EOF
$libraries
EOF

for file in .github/release/README.txt LICENSE .github/release/LICENSES.txt tools/N64ModernRuntime/COPYING; do
  [ -f "$file" ] || fail "Missing file: $file"
done

echo "==> Copying license and readme files"
cp .github/release/README.txt LICENSE .github/release/LICENSES.txt "$STAGE/"
cp tools/N64ModernRuntime/COPYING "$STAGE/LICENSE-GPL-3.0.txt"

echo "==> Writing launcher script"
cat <<'EOF' > "$STAGE/ConkerRecomp"
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
cd "$here" || exit 1
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/ConkerRecomp.bin" "$@"
EOF
chmod 755 "$STAGE/ConkerRecomp"

echo "==> Performing safety check for ROMs and data folders"
safety_check=$(find "$STAGE" \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -name 'conker_data' -o -name 'baserom*' \))
if [ -n "$safety_check" ]; then
  printf '%s\n' "$safety_check" >&2
  fail "Found disallowed files/folders"
fi

echo "==> Creating tarball"
mkdir -p "$out_dir"
tar -czf "$out_dir/ConkerRecompiled-$tag-Linux.tar.gz" -C "$out_dir" ConkerRecompiled
echo "==> Tarball created: $out_dir/ConkerRecompiled-$tag-Linux.tar.gz"
ls -l "$out_dir/ConkerRecompiled-$tag-Linux.tar.gz"
sha256sum "$out_dir/ConkerRecompiled-$tag-Linux.tar.gz"
