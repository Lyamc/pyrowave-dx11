// vkref_driver: one Vulkan encode of a YUV420P frame from tools/genvectors.py.
// Links the patched pyrowave C API. Does not generate pixels itself.

#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
	fprintf(stderr,
	        "usage: pyrowave-vkref-driver --yuv FILE --width W --height H --vector NAME\n"
	        "       --budget BYTES --dump-dir DIR --dump on|off\n"
	        "\n"
	        "The YUV file is one YUV420P frame written by tools/genvectors.py\n"
	        "(Y plane, then Cb, then Cr, no header). --dump off leaves DIR empty.\n");
}

static const char *need_arg(int *i, int argc, char **argv)
{
	if (*i + 1 >= argc)
	{
		fprintf(stderr, "missing value after %s\n", argv[*i]);
		exit(2);
	}
	*i += 1;
	return argv[*i];
}

int main(int argc, char **argv)
{
	const char *yuv_path = NULL;
	const char *vector_name = NULL;
	const char *dump_dir = NULL;
	const char *dump_mode = NULL;
	int width = 0;
	int height = 0;
	unsigned long long budget = 0;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--yuv") == 0)
			yuv_path = need_arg(&i, argc, argv);
		else if (strcmp(argv[i], "--width") == 0)
			width = atoi(need_arg(&i, argc, argv));
		else if (strcmp(argv[i], "--height") == 0)
			height = atoi(need_arg(&i, argc, argv));
		else if (strcmp(argv[i], "--vector") == 0)
			vector_name = need_arg(&i, argc, argv);
		else if (strcmp(argv[i], "--budget") == 0)
			budget = strtoull(need_arg(&i, argc, argv), NULL, 10);
		else if (strcmp(argv[i], "--dump-dir") == 0)
			dump_dir = need_arg(&i, argc, argv);
		else if (strcmp(argv[i], "--dump") == 0)
			dump_mode = need_arg(&i, argc, argv);
		else
		{
			fprintf(stderr, "unknown argument %s\n", argv[i]);
			usage();
			return 2;
		}
	}

	if (!yuv_path || !vector_name || !dump_dir || !dump_mode || width <= 0 || height <= 0 || budget == 0)
	{
		usage();
		return 2;
	}
	if (width % 2 || height % 2)
	{
		fprintf(stderr, "YUV420P width and height must be even\n");
		return 2;
	}

	int dump_on = strcmp(dump_mode, "on") == 0;
	if (!dump_on && strcmp(dump_mode, "off") != 0)
	{
		fprintf(stderr, "--dump must be on or off\n");
		return 2;
	}

	size_t y_bytes = (size_t)width * (size_t)height;
	size_t c_bytes = y_bytes / 4;
	size_t total = y_bytes + c_bytes * 2;
	FILE *fp = fopen(yuv_path, "rb");
	if (!fp)
	{
		fprintf(stderr, "cannot open %s\n", yuv_path);
		return 1;
	}
	unsigned char *frame = (unsigned char *)malloc(total);
	if (!frame)
	{
		fclose(fp);
		return 1;
	}
	size_t got = fread(frame, 1, total, fp);
	fclose(fp);
	if (got != total)
	{
		fprintf(stderr, "yuv is %zu bytes, expected %zu\n", got, total);
		free(frame);
		return 1;
	}

	pyrowave_device device = NULL;
	pyrowave_result result = pyrowave_create_default_device(&device);
	if (result != PYROWAVE_SUCCESS)
	{
		fprintf(stderr, "pyrowave_create_default_device failed (%d)\n", (int)result);
		free(frame);
		return 1;
	}

	pyrowave_encoder_create_info info = {};
	info.device = device;
	info.width = width;
	info.height = height;
	info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

	pyrowave_encoder encoder = NULL;
	result = pyrowave_encoder_create(&info, &encoder);
	if (result != PYROWAVE_SUCCESS)
	{
		fprintf(stderr, "pyrowave_encoder_create failed (%d)\n", (int)result);
		pyrowave_device_destroy(device);
		free(frame);
		return 1;
	}

	if (dump_on)
		pyrowave_encoder_set_dump_dir(encoder, dump_dir, vector_name);
	else
		pyrowave_encoder_set_dump_dir(encoder, "", vector_name);

	pyrowave_cpu_buffer cpu = {};
	cpu.data[0] = frame;
	cpu.data[1] = frame + y_bytes;
	cpu.data[2] = frame + y_bytes + c_bytes;
	cpu.row_stride_in_bytes[0] = (size_t)width;
	cpu.row_stride_in_bytes[1] = (size_t)width / 2;
	cpu.row_stride_in_bytes[2] = (size_t)width / 2;
	cpu.plane_size_in_bytes[0] = y_bytes;
	cpu.plane_size_in_bytes[1] = c_bytes;
	cpu.plane_size_in_bytes[2] = c_bytes;
	cpu.width = width;
	cpu.height = height;
	cpu.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

	pyrowave_rate_control rate = {};
	rate.maximum_bitstream_size = (size_t)budget;

	result = pyrowave_encoder_encode_cpu(encoder, &cpu, &rate);
	fprintf(stdout, "encode_cpu %d vector %s %dx%d budget %llu dump %s\n",
	        (int)result, vector_name, width, height, budget, dump_on ? "on" : "off");

	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
	free(frame);
	return result == PYROWAVE_SUCCESS ? 0 : 1;
}
