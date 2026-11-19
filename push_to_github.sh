#!/bin/bash
# Push clean repo to rxresiire/Quest-2-kexec (or similar)
# Run from PROJECT_ASTRO/clean_repo

set -e

REPO_OWNER="rxresiire"
REPO_NAME="Quest-2-kexec"
REMOTE_URL="https://github.com/$REPO_OWNER/$REPO_NAME.git"

echo "Initializing git repository..."
git init
git config user.name "Kilo"
git config user.email "kilo@kilo.ai"

echo "Adding all files..."
git add .

echo "Committing..."
git commit -m "Quest 2 (Hollywood) kexec module for cip132-st16

- loader.c: DMA gate removed for Hollywood; qkx_execute() integrates shutdown framework
- shutdown_backends.c: NEW - 10 Quest 2 backends (CAMERA, DSP, GPU, SYNCBOSS, USB, UFS, OTHER_DMA, SMMU, SECURE, AOP)
- Makefile: +shutdown_backends.o
- GitHub Actions workflow: builds targeting oculus-quest2-kernel-master (build 52322490022300150)
- Default: QKX_EXPERIMENTAL=0 (staging only, no hardware transition)
- validate_and_install.sh: passive checks + reversible tests only
- deploy_and_verify.sh: local verification + Magisk staging install

Safety: no partition writes, no bootloader changes, Magisk preserved, SELinux permissive"

echo "Checking if gh CLI is available..."
if ! command -v gh &> /dev/null; then
    echo "gh CLI not found. Please install it first."
    exit 1
fi

echo "Checking gh auth status..."
gh auth status || { echo "Please run 'gh auth login' first"; exit 1; }

echo "Creating repository if it doesn't exist..."
gh repo create "$REPO_OWNER/$REPO_NAME" --private --description "Quest 2 (Hollywood) kexec module for cip132-st16 kernel - staging only, no hardware transition" || echo "Repo may already exist"

echo "Adding remote..."
git remote add origin "$REMOTE_URL" 2>/dev/null || git remote set-url origin "$REMOTE_URL"

echo "Pushing to main branch..."
git push -u origin main

echo "Done! Repository pushed to $REMOTE_URL"
echo ""
echo "Next steps:"
echo "1. Trigger build: gh workflow run quest2-kexec-cip132-build -R $REPO_OWNER/$REPO_NAME -f experimental=false"
echo "2. Watch build: gh run watch -R $REPO_OWNER/$REPO_NAME"
echo "3. Download artifacts: gh run download -R $REPO_OWNER/$REPO_NAME"
echo "4. Run deploy_and_verify.sh"