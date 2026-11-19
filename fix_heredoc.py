import re

with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'r') as f:
    content = f.read()

# Fix first heredoc
pattern1 = r"(cat > \$\{\{ runner\.temp \}\}/generate_crcs\.py << 'PYEOF'\n)(.*?)(\nPYEOF)"
def fix_heredoc(match):
    header = match.group(1)
    body = match.group(2)
    footer = match.group(3)
    lines = body.split('\n')
    fixed_lines = []
    for line in lines:
        if line.strip():
            fixed_lines.append('          ' + line)
        else:
            fixed_lines.append(line)
    return header + '\n'.join(fixed_lines) + footer

content = re.sub(pattern1, fix_heredoc, content, flags=re.DOTALL)

# Fix second heredoc
pattern2 = r"(cat > \$\{\{ runner\.temp \}\}/generate_provenance\.py << 'PYEOF'\n)(.*?)(\nPYEOF)"
content = re.sub(pattern2, fix_heredoc, content, flags=re.DOTALL)

with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'w') as f:
    f.write(content)

print("Fixed")