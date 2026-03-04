#!/bin/bash
# AlJefra OS — Website Deploy Script
#
# Rebuilds the Docker container and deploys the website to os.aljefra.com.
# The site is served via nginx container on port 8095, exposed through
# Cloudflare Tunnel.
#
# Usage:
#   ./website/deploy.sh          # Build and deploy
#   ./website/deploy.sh --build  # Build only (don't restart)
#   ./website/deploy.sh --check  # Check current status

set -e

CONTAINER_NAME="aljefra-os-website"
IMAGE_NAME="aljefra-os-website"
PORT=8095
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[deploy]${NC} $1"; }
warn()  { echo -e "${YELLOW}[deploy]${NC} $1"; }
error() { echo -e "${RED}[deploy]${NC} $1"; }

# --- Status check ---
if [ "$1" = "--check" ]; then
    echo "=== AlJefra OS Website Status ==="
    if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
        info "Container: Running"
        docker ps --filter "name=${CONTAINER_NAME}" --format "  ID: {{.ID}}\n  Image: {{.Image}}\n  Up: {{.Status}}\n  Ports: {{.Ports}}"
    else
        error "Container: Not running"
    fi
    echo ""
    info "Testing local response..."
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:${PORT}/ 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ]; then
        info "Local (localhost:${PORT}): OK (HTTP $HTTP_CODE)"
    else
        error "Local (localhost:${PORT}): FAIL (HTTP $HTTP_CODE)"
    fi
    echo ""
    info "Testing live site..."
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" https://os.aljefra.com/ 2>/dev/null || echo "000")
    if [ "$HTTP_CODE" = "200" ]; then
        info "Live (os.aljefra.com): OK (HTTP $HTTP_CODE)"
    else
        error "Live (os.aljefra.com): FAIL (HTTP $HTTP_CODE)"
    fi
    exit 0
fi

# --- Auto-generate pages from source ---
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
info "Regenerating roadmap.html from ROADMAP.md..."
python3 "${REPO_ROOT}/tools/gen_roadmap.py"

info "Running doc_check --fix for numeric consistency..."
python3 "${REPO_ROOT}/tools/doc_check.py" --fix || true

# --- Build ---
info "Building Docker image from ${SCRIPT_DIR}..."
docker build -t "${IMAGE_NAME}:latest" "${SCRIPT_DIR}"
info "Image built: ${IMAGE_NAME}:latest"

if [ "$1" = "--build" ]; then
    info "Build-only mode. Skipping deploy."
    exit 0
fi

# --- Deploy ---
info "Stopping old container..."
docker stop "${CONTAINER_NAME}" 2>/dev/null || true
docker rm "${CONTAINER_NAME}" 2>/dev/null || true

info "Starting new container on port ${PORT}..."
docker run \
    --restart=always \
    -d \
    -p "${PORT}:80" \
    --name "${CONTAINER_NAME}" \
    "${IMAGE_NAME}:latest"

# --- Verify ---
sleep 2
info "Verifying deployment..."

HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:${PORT}/ 2>/dev/null || echo "000")
if [ "$HTTP_CODE" = "200" ]; then
    info "Local check: OK (HTTP $HTTP_CODE)"
else
    error "Local check: FAIL (HTTP $HTTP_CODE)"
    exit 1
fi

# Check that roadmap page was auto-generated (has the marker comment)
if curl -s http://localhost:${PORT}/roadmap.html | grep -q "auto-generated from ROADMAP.md"; then
    info "Roadmap content: Auto-generated and current"
else
    warn "Roadmap content: May not be auto-generated (check gen_roadmap.py)"
fi

echo ""
info "Deploy complete! Site live at https://os.aljefra.com"
