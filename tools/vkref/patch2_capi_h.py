with open("pyrowave.h", "r") as f:
    content = f.read()

# Add encode_bands C API declaration after encode_cpu
old = """// Encode where pixel data is provided on CPU. Encoding still happens on GPU of course.
// A command buffer must not be set on pyrowave_device.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_cpu(pyrowave_encoder encoder, const pyrowave_cpu_buffer *buffers,
                             const pyrowave_rate_control *rate_control);

// Can only be called after a successful encoding operation"""

new = """// Encode where pixel data is provided on CPU. Encoding still happens on GPU of course.
// A command buffer must not be set on pyrowave_device.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_cpu(pyrowave_encoder encoder, const pyrowave_cpu_buffer *buffers,
                             const pyrowave_rate_control *rate_control);

// Encode bands already in the wavelet images (T6a band injection for validation).
// bands_f32 must contain f32 data for all bands in the same order as the DWT output.
// count is the total number of f32 values. The driver interprets the layout.
// Increments sequence_count like encode_cpu does.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_bands(pyrowave_encoder encoder, const float *bands_f32, size_t count,
                               const pyrowave_rate_control *rate_control);

// Can only be called after a successful encoding operation"""

content = content.replace(old, new)

with open("pyrowave.h", "w") as f:
    f.write(content)

print("Patched pyrowave.h with encode_bands C API")
