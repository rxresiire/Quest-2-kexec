with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'r') as f:
    lines = f.readlines()

# State machine: track when we're inside a heredoc that needs fixing
in_heredoc = False
heredoc_indent = 0
new_lines = []

for line in lines:
    stripped = line.rstrip('\n\r')
    
    # Check for heredoc start
    if '<< \'PYEOF\'' in stripped and stripped.rstrip().endswith("<< 'PYEOF'"):
        in_heredoc = True
        # Calculate the indentation of this line
        heredoc_indent = len(stripped) - len(stripped.lstrip())
        new_lines.append(line)
        continue
    
    # Check for heredoc end
    if in_heredoc and stripped == 'PYEOF':
        in_heredoc = False
        # Fix the PYEOF line indentation
        new_lines.append(' ' * heredoc_indent + 'PYEOF\n')
        continue
    
    # Inside heredoc - fix indentation
    if in_heredoc:
        if stripped.strip():  # Non-empty line
            new_lines.append(' ' * heredoc_indent + stripped + '\n')
        else:
            new_lines.append('\n')
        continue
    
    new_lines.append(line)

with open(r'C:\Users\Administrator\AppData\Roaming\Kilo Desktop\chats\session-G7eQoY\PROJECT_ASTRO\clean_repo\.github\workflows\quest2-kexec-cip132-build.yml', 'w') as f:
    f.writelines(new_lines)

print("Fixed")