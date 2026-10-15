with open("pyrowave_encoder.cpp", "r") as f:
    content = f.read()

# Add encode_bands to Impl struct (after encode_pre_transformed declaration)
old_impl = "\tbool encode_pre_transformed(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);\n\tbool encode_quant_and_coding(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);\n"
new_impl = """\tbool encode_pre_transformed(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
\tbool encode_quant_and_coding(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
\tbool encode_bands(CommandBuffer &cmd, const BitstreamBuffers &buffers);"""

content = content.replace(old_impl, new_impl)

# Add encode_bands implementation after encode_pre_transformed
old_ep = """bool Encoder::Impl::encode_pre_transformed(
\t\tVulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
\tcmd.fill_buffer(*payload_data, 0, 0, 2 * sizeof(uint32_t));
\tcmd.fill_buffer(*bucket_buffer, 0);
\tcmd.fill_buffer(*quant_buffer, 0);

\t// Don't need to read the payload offset counter until quantizer.
\tcmd.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
\t            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
\t            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

\treturn encode_quant_and_coding(cmd, buffers, quant_scale);
}"""

new_ep = """bool Encoder::Impl::encode_pre_transformed(
\t\tVulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
\tcmd.fill_buffer(*payload_data, 0, 0, 2 * sizeof(uint32_t));
\tcmd.fill_buffer(*bucket_buffer, 0);
\tcmd.fill_buffer(*quant_buffer, 0);

\t// Don't need to read the payload offset counter until quantizer.
\tcmd.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
\t            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
\t            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

\treturn encode_quant_and_coding(cmd, buffers, quant_scale);
}

bool Encoder::Impl::encode_bands(CommandBuffer &cmd, const BitstreamBuffers &buffers)
{
\t// Don't increment sequence_count here - the driver does that.
\t// encode_pre_transformed also doesn't increment it.
\t// This matches the upstream behavior where only encode() increments sequence_count.

\tcmd.fill_buffer(*payload_data, 0, 0, 2 * sizeof(uint32_t));
\tcmd.fill_buffer(*bucket_buffer, 0);
\tcmd.fill_buffer(*quant_buffer, 0);

\tcmd.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
\t            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
\t            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

\treturn encode_quant_and_coding(cmd, buffers, -1.0f);
}"""

content = content.replace(old_ep, new_ep)

# Add public encode_bands method after encode_pre_transformed
old_pub = """bool Encoder::encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
\treturn impl->encode_pre_transformed(cmd, buffers, quant_scale);
}"""

new_pub = """bool Encoder::encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
\treturn impl->encode_pre_transformed(cmd, buffers, quant_scale);
}

bool Encoder::encode_bands(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers)
{
\treturn impl->encode_bands(cmd, buffers);
}"""

content = content.replace(old_pub, new_pub)

with open("pyrowave_encoder.cpp", "w") as f:
    f.write(content)

print("Patched pyrowave_encoder.cpp")
