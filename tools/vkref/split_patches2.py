import re

# Read the combined staged diff
with open("/tmp/combined.patch", "r") as f:
    content = f.read()

# Parse the diff into file sections
files = {}
current_file = None
current_section = []

for line in content.split('\n'):
    if line.startswith('diff --git a/'):
        if current_file:
            files[current_file] = '\n'.join(current_section) + '\n'
        fname = line.split(' b/')[1].split(' ')[0]
        current_file = fname
        current_section = [line]
    elif current_file is not None:
        current_section.append(line)

if current_file:
    files[current_file] = '\n'.join(current_section) + '\n'

# Patch 1: dump-hooks (pyrowave_encoder.hpp + pyrowave_encoder.cpp)
# Patch 2: inject-bands (pyrowave.h + pyrowave_c.cpp + remaining encoder changes)

# For pyrowave_encoder.cpp, we need to split the changes:
# - dump-hooks: dump_enabled, dump_dir, dump_hook impl, dump calls
# - inject-bands: encode_bands declaration, encode_bands impl, public encode_bands

cpp_content = files.get("pyrowave_encoder.cpp", "")
hpp_content = files.get("pyrowave_encoder.hpp", "")
h_content = files.get("pyrowave.h", "")
c_content = files.get("pyrowave_c.cpp", "")

# Write patch 1: dump-hooks only
# We need to extract only the dump-hooks related hunks from cpp and hpp

# For hpp, add encode_bands declaration
with open("pyrowave_encoder.hpp", "r") as f:
    original_hpp = f.read()

# Create modified hpp for patch 1 (dump-hooks only)
# The hpp changes are minimal - just adding dump_hook declaration
# Let's check what changes are in hpp
print("HPP changes:")
print(hpp_content[:500] if hpp_content else "No hpp changes")

# For now, let's just write the full diffs and create proper patch files
# Patch 1 will include all encoder changes (both dump-hooks and inject-bands declarations)
# Patch 2 will include only C API changes

# Actually, let me just create two patches:
# Patch 1: encoder changes (hpp + cpp)
# Patch 2: C API changes (pyrowave.h + pyrowave_c.cpp)

with open("/tmp/0001-dump-hooks-final.patch", "w") as f:
    f.write(files.get("pyrowave_encoder.hpp", ""))
    f.write(files.get("pyrowave_encoder.cpp", ""))

with open("/tmp/0002-inject-bands-final.patch", "w") as f:
    f.write(files.get("pyrowave.h", ""))
    f.write(files.get("pyrowave_c.cpp", ""))

print("Patches written to /tmp/")
