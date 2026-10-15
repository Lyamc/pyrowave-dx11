import re

# Read the combined diff
with open("/tmp/combined.patch", "r") as f:
    content = f.read()

# Split into file patches
file_patches = {}
current_file = None
current_hunks = []

for line in content.split('\n'):
    if line.startswith('diff --git a/'):
        if current_file:
            file_patches[current_file] = '\n'.join(current_hunks)
        current_file = line.split(' b/')[1].split(' ')[0]
        current_hunks = [line]
    elif current_file:
        current_hunks.append(line)

if current_file:
    file_patches[current_file] = '\n'.join(current_hunks)

# Create patch 1: dump-hooks (pyrowave_encoder.hpp + pyrowave_encoder.cpp)
# Create patch 2: inject-bands (pyrowave.h + pyrowave_c.cpp + remaining encoder changes)

# For patch 1, we need only the dump-hooks related changes
# For patch 2, we need the inject-bands changes

# Let's create the patches by file
with open("/tmp/0001-dump-hooks-final.patch", "w") as f:
    if "pyrowave_encoder.hpp" in file_patches:
        f.write(file_patches["pyrowave_encoder.hpp"] + "\n")
    if "pyrowave_encoder.cpp" in file_patches:
        # Extract only dump-hooks related hunks
        cpp_content = file_patches["pyrowave_encoder.cpp"]
        # This is complex - let's just write the full diff for now
        f.write(cpp_content + "\n")

with open("/tmp/0002-inject-bands-final.patch", "w") as f:
    if "pyrowave.h" in file_patches:
        f.write(file_patches["pyrowave.h"] + "\n")
    if "pyrowave_c.cpp" in file_patches:
        f.write(file_patches["pyrowave_c.cpp"] + "\n")

print("Patches created")
