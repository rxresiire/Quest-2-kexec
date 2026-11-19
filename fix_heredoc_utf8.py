with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'r', encoding='utf-8') as f:
    lines = f.readlines()

in_heredoc = False
heredoc_indent = 0
new_lines = []

for line in lines:
    stripped = line.rstrip('\n\r')
    
    # Check for heredoc start
    if '<< \'PYEOF\'' in stripped and stripped.rstrip().endswith("<< 'PYEOF'"):
        in_heredoc = True
        heredoc_indent = len(stripped) - len(stripped.lstrip())
        new_lines.append(line)
        continue
    
    # Check for heredoc end
    if in_heredoc and stripped == 'PYEOF':
        in_heredoc = False
        new_lines.append(' ' * heredoc_indent + 'PYEOF\n')
        continue
    
    # Inside heredoc - fix ALL lines (including blank)
    if in_heredoc:
        new_lines.append(' ' * heredoc_indent + stripped + '\n')
        continue
    
    new_lines.append(line)

with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'w', encoding='utf-8', newline='\n') as f:
    f.writelines(new_lines)

print("Fixed")