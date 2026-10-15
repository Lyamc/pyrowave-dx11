with open("pyrowave_encoder.cpp", "r") as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    if "fopen" in line and "path" in line and '"wb"' not in line:
        lines[i] = '\tFILE *f = fopen(path.c_str(), "wb");\n'
        print(f"Fixed line {i+1}: {lines[i].rstrip()}")

with open("pyrowave_encoder.cpp", "w") as f:
    f.writelines(lines)
print("Done")
