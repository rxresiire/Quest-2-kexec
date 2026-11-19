@echo off
cd /d "C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo"
"C:\Program Files\Git\bin\git.exe" add .github\workflows\quest2-kexec-cip132-build.yml
"C:\Program Files\Git\bin\git.exe" commit -m "Fix workflow: copy pre-modified files instead of inline patching"
"C:\Program Files\Git\bin\git.exe" push