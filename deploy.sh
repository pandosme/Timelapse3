#!/usr/bin/env bash
set -u

APP_NAME="${APP_NAME:-timelapse2}"
CAMERA_USER="${CAMERA_USER:-nodered}"
CAMERA_PASS="${CAMERA_PASS:-rednode}"
CAMERA_SCHEME="${CAMERA_SCHEME:-http}"
PACKAGE="${PACKAGE:-}"

CAMERAS=(
    front.internal
    back.internal
    driveway.internal
    parking.internal
)

if [[ -z "${PACKAGE}" ]]; then
    mapfile -t packages < <(find . -maxdepth 1 -type f -name 'Timelapse2_*_aarch64.eap' -printf '%f\n' | sort -V)
    if [[ ${#packages[@]} -gt 0 ]]; then
        PACKAGE="${packages[-1]}"
    fi
fi

if [[ -z "${PACKAGE}" || ! -f "${PACKAGE}" ]]; then
    echo "No aarch64 EAP package found. Build first with ./build.sh or set PACKAGE=/path/to/app.eap" >&2
    exit 1
fi

netrc_file="$(mktemp)"
trap 'rm -f "${netrc_file}"' EXIT
chmod 600 "${netrc_file}"

failures=0

for camera in "${CAMERAS[@]}"; do
    cat >"${netrc_file}" <<EOF
machine ${camera}
login ${CAMERA_USER}
password ${CAMERA_PASS}
EOF

    base_url="${CAMERA_SCHEME}://${camera}"
    echo "==> ${camera}: stopping ${APP_NAME} if running"
    curl --silent --show-error --anyauth --netrc-file "${netrc_file}" \
        --connect-timeout 10 --max-time 30 \
        "${base_url}/axis-cgi/applications/control.cgi?action=stop&package=${APP_NAME}" >/dev/null || true

    echo "==> ${camera}: uploading ${PACKAGE}"
    if ! curl --silent --show-error --fail --anyauth --netrc-file "${netrc_file}" \
        --connect-timeout 10 --max-time 300 \
        -F "packfil=@${PACKAGE}" \
        "${base_url}/axis-cgi/applications/upload.cgi" >/dev/null; then
        echo "ERROR: ${camera}: upload failed" >&2
        failures=$((failures + 1))
        continue
    fi

    echo "==> ${camera}: starting ${APP_NAME}"
    if ! curl --silent --show-error --fail --anyauth --netrc-file "${netrc_file}" \
        --connect-timeout 10 --max-time 60 \
        "${base_url}/axis-cgi/applications/control.cgi?action=start&package=${APP_NAME}" >/dev/null; then
        echo "ERROR: ${camera}: start failed" >&2
        failures=$((failures + 1))
        continue
    fi

    echo "==> ${camera}: deployed"
done

if [[ ${failures} -gt 0 ]]; then
    echo "Completed with ${failures} failed camera(s)." >&2
    exit 1
fi

echo "All cameras deployed successfully."