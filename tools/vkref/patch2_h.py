with open("pyrowave.h", "r") as f:
    content = f.read()

# Add encode_bands declaration after encode_pre_transformed (in the debug section)
old = "\t// Debug\n\tvoid report_stats(const void *mapped_meta, const void *mapped_bitstream) const;\n"
new = """\t// Debug
\tvoid report_stats(const void *mapped_meta, const void *mapped_bitstream) const;
\t// Encode bands already in the wavelet images (T6a band injection).
\tbool encode_bands(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers);"""

content = content.replace(old, new)

with open("pyrowave.h", "w") as f:
    f.write(content)

print("Patched pyrowave.h")
