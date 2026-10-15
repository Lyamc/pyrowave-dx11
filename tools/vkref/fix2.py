with open("pyrowave_encoder.cpp", "r") as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    if "dump_dir" in line and "path" in line and "bin" in line:
        lines[i] = '\tstd::string path = dump_dir + "/" + name + ".bin";\n'
        print(f"Fixed line {i+1}")
    if "fopen" in line and "path" in line and "wb" not in line:
        lines[i] = '\tFILE *f = fopen(path.c_str(), "wb");\n'
        print(f"Fixed line {i+1}")

with open("pyrowave_encoder.cpp", "w") as f:
    f.writelines(lines)
print("Done")
