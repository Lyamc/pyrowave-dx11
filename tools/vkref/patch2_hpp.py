with open("pyrowave_encoder.hpp", "r") as f:
    content = f.read()

# Add encode_bands declaration after encode_pre_transformed
old = "\tbool encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);\n\t//\n"
new = """\tbool encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
\t//
\t// Encode bands already in the wavelet images (T6a band injection).
\tbool encode_bands(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers);
\t//"""

content = content.replace(old, new)

with open("pyrowave_encoder.hpp", "w") as f:
    f.write(content)

print("Patched pyrowave_encoder.hpp")
