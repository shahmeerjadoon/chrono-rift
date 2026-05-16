#!/usr/bin/env bash
# Build image and open an interactive shell with the repo at /app.
# Mounts X11 so SFML HIP can open a window from the container (Linux host).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${CHRONO_IMAGE:-chrono-rift}"

docker build -t "$IMAGE" "$ROOT"

# LIBGL_ALWAYS_SOFTWARE=1: Mesa uses llvmpipe instead of iris/DRI — fixes drm/iris spam in GPU-less Docker.
# CHRONO_VSYNC=0: HIP skips VSync so SFML does not print "vertical sync not supported" on software GL.
exec docker run --rm -it \
  -v "$ROOT:/app" \
  -w /app \
  -e "DISPLAY=${DISPLAY:-:0}" \
  -e LIBGL_ALWAYS_SOFTWARE=1 \
  -e CHRONO_VSYNC=0 \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  "$IMAGE" bash -lc 'echo "In container: cd /app && make && ./arbiter_exec"; exec bash'
