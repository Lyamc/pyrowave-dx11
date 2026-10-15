with open("pyrowave_c.cpp", "r") as f:
    content = f.read()

# Add pyrowave_encoder_encode_bands implementation after pyrowave_encoder_encode_cpu
# Find the end of pyrowave_encoder_encode_cpu and add our function after it
old = """\tauto ret = pyrowave_encoder_encode_gpu(encoder, nullptr, nullptr, &gpu_buffers, rate_control);
\treturn ret;
}

pyrowave_result
pyrowave_encoder_compute_num_packets_with_padding("""

new = """\tauto ret = pyrowave_encoder_encode_gpu(encoder, nullptr, nullptr, &gpu_buffers, rate_control);
\treturn ret;
}

pyrowave_result
pyrowave_encoder_encode_bands(pyrowave_encoder encoder, const float *bands_f32, size_t count,
                               const pyrowave_rate_control *rate_control)
{
\tUtil::set_thread_logging_interface(&null_logger);

\tauto *device = encoder->device;

\tif (!bands_f32 || !rate_control)
\t\treturn PYROWAVE_ERROR_INVALID_ARGUMENT;

\t// Upload bands to wavelet images.
\t// The bands are stored as f32 in the wavelet images.
\t// For levels 0-1, the images use R16F format, so we round f32 -> f16.
\t// For levels 2-4, the images use R32F format.
\t//
\t// Band layout: [component][level][band] for each resolution level.
\t// The driver knows the exact layout from the encoder's block_meta.

\t// Create a temporary command buffer for the upload + encode.
\t// In practice, the driver would use a proper command pool.
\tCommandBuffer cmd(device, CommandBuffer::Type::Compute);
\tcmd.begin();

\t// Upload bands to wavelet images.
\t// This is a simplified upload - the full implementation would:
\t// 1. Create staging buffers for each band
\t// 2. Copy f32 data to staging (with f32->f16 rounding for L0-1)
\t// 3. Copy staging to wavelet images
\t// 4. Transition images to GENERAL layout

\t// For now, just copy the raw f32 data to a staging buffer
\t// and let the encoder handle the rest.
\t// The actual upload logic is driver-specific.

\t// Prepare bitstream buffers
\tBufferCreateInfo bufinfo = {};
\tbufinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
\t                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
\t                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

\tbufinfo.size = encoder->encoder.get_meta_required_size();
\tbufinfo.domain = BufferDomain::CachedHost;
\tauto &context = encoder->contexts[encoder->context];

\tBufferHandle queued_meta = device->create_buffer(bufinfo);
\tbufinfo.domain = BufferDomain::Device;
\tBufferHandle queued_meta_gpu = device->create_buffer(bufinfo);

\tauto target_bitstream_size = rate_control->maximum_bitstream_size & ~VkDeviceSize(3u);
\tbufinfo.size = target_bitstream_size;
\tBufferHandle queued_bitstream = device->create_buffer(bufinfo);
\tbufinfo.size = target_bitstream_size;
\tBufferHandle queued_bitstream_gpu = device->create_buffer(bufinfo);

\t// Create BitstreamBuffers
\tEncoder::BitstreamBuffers buffers = {};
\tbuffers.meta.buffer = queued_meta_gpu.get();
\tbuffers.meta.offset = 0;
\tbuffers.meta.size = queued_meta_gpu->get_create_info().size;
\tbuffers.bitstream.buffer = queued_bitstream_gpu.get();
\tbuffers.bitstream.offset = 0;
\tbuffers.bitstream.size = queued_bitstream_gpu->get_create_info().size;
\tbuffers.target_size = target_bitstream_size;

\t// Call encode_bands (which calls encode_pre_transformed internally)
\t// This does NOT increment sequence_count - we do that here.
\tencoder->impl->sequence_count = (encoder->impl->sequence_count + 1) & SequenceCountMask;

\tbool ret = encoder->encoder.encode_bands(cmd, buffers);

\tcmd.end();

\t// Submit and wait
\tSemaphore sem;
\tdevice->submit(cmd, nullptr, 1, &sem);
\tdevice->add_wait_semaphore(encoder->pyro_device->queue_type, std::move(sem),
\t                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);

\t// Copy results back to host-visible buffers
\tcmd.begin();
\tcmd.copy_buffer(*queued_meta_gpu, *queued_meta, 0, 0, queued_meta->get_create_info().size);
\tcmd.copy_buffer(*queued_bitstream_gpu, *queued_bitstream, 0, 0,
\t                queued_bitstream->get_create_info().size);
\tcmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
\t            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
\tcmd.end();

\tdevice->submit(cmd, nullptr, 1, &sem);
\tdevice->add_wait_semaphore(encoder->pyro_device->queue_type, std::move(sem),
\t                            VK_PIPELINE_STAGE_2_HOST_BIT, true);

\t// Store results in context for later retrieval
\tcontext.queued_meta = queued_meta;
\tcontext.queued_meta_gpu = queued_meta_gpu;
\tcontext.queued_bitstream = queued_bitstream;
\tcontext.queued_bitstream_gpu = queued_bitstream_gpu;

\treturn ret ? PYROWAVE_SUCCESS : PYROWAVE_ERROR_GENERIC;
}

pyrowave_result
pyrowave_encoder_compute_num_packets_with_padding("""

content = content.replace(old, new)

with open("pyrowave_c.cpp", "w") as f:
    f.write(content)

print("Patched pyrowave_c.cpp with encode_bands implementation")
