# embed_shader.py -- add size constant to /Fh-generated header
import sys
import re

hdr_path = sys.argv[1]
var_name = sys.argv[2]

with open(hdr_path, 'r') as f:
    content = f.read()

# Find the array definition: const BYTE g_name_dxbc[] = \n { ... };
# The /Fh output has the opening brace on a separate line
pattern = r'(const BYTE ' + re.escape(var_name) + r'\[\]\s*=\s*\n\s*\{)([^}]+)(\};)'
match = re.search(pattern, content, re.DOTALL)
if not match:
    print(f"ERROR: Could not find array definition for {var_name}", file=sys.stderr)
    sys.exit(1)

# Count the bytes
byte_str = match.group(2)
bytes_list = [b.strip() for b in byte_str.split(',') if b.strip()]
num_bytes = len(bytes_list)

# Add size constant after the array definition
replacement = match.group(1) + match.group(2) + match.group(3) + f'\nconst size_t {var_name}_len = {num_bytes};'
content = content.replace(match.group(0), replacement)

with open(hdr_path, 'w') as f:
    f.write(content)

print(f"Added size constant: {var_name}_len = {num_bytes}")
