# Push clean repo to rxresiire/Quest-2-kexec (or similar)
# Run from PROJECT_ASTRO\clean_repo in PowerShell

$ErrorActionPreference = "Stop"

$REPO_OWNER = "rxresiire"
$REPO_NAME = "Quest-2-kexec"
$REMOTE_URL = "https://github.com/$REPO_OWNER/$REPO_NAME.git"

Write-Host "Initializing git repository..." -ForegroundColor Cyan
git init
git config user.name "Kilo"
git config user.email "kilo@kilo.ai"

Write-Host "Adding all files..." -ForegroundColor Cyan
git add .

Write-Host "Committing..." -ForegroundColor Cyan
git commit -m "Quest 2 (Hollywood) kexec module for cip132-st16

- loader.c: DMA gate removed for Hollywood; qkx_execute() integrates shutdown framework
- shutdown_backends.c: NEW - 10 Quest 2 backends (CAMERA, DSP, GPU, SYNCBOSS, USB, UFS, OTHER_DMA, SMMU, SECURE, AOP)
- Makefile: +shutdown_backends.o
- GitHub Actions workflow: builds targeting oculus-quest2-kernel-master (build 52322490022300150)
- Default: QKX_EXPERIMENTAL=0 (staging only, no hardware transition)
- validate_and_install.sh: passive checks + reversible tests only
- deploy_and_verify.sh: local verification + Magisk staging install

Safety: no partition writes, no bootloader changes, Magisk preserved, SELinux permissive"

Write-Host "Checking if gh CLI is available..." -ForegroundColor Cyan
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Error "gh CLI not found. Please install it first."
    exit 1
}

Write-Host "Checking gh auth status..." -ForegroundColor Cyan
try {
    gh auth status
} catch {
    Write-Error "Please run 'gh auth login' first"
    exit 1
}

Write-Host "Creating repository if it doesn't exist..." -ForegroundColor Cyan
try {
    gh repo create "$REPO_OWNER/$REPO_NAME" --private --description "Quest 2 (Hollywood) kexec module for cip132-st16 kernel - staging only, no hardware transition"
} catch {
    Write-Warning "Repo may already exist or creation failed"
}

Write-Host "Adding remote..." -ForegroundColor Cyan
try {
    git remote add origin $REMOTE_URL
} catch {
    git remote set-url origin $REMOTE_URL
}

Write-Host "Pushing to main branch..." -ForegroundColor Cyan
git push -u origin main

Write-Host "Done! Repository pushed to $REMOTE_URL" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Trigger build: gh workflow run quest2-kexec-cip132-build -R $REPO_OWNER/$REPO_NAME -f experimental=false"
Write-Host "2. Watch build: gh run watch -R $REPO_OWNER/$REPO_NAME"
Write-Host "3. Download artifacts: gh run download -R $REPO_OWNER/$REPO_NAME"
Write-Host "4. Run deploy_and_verify.sh"