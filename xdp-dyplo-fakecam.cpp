/*
 * Dyplo example video application for XDP
 *
 * (C) Copyright 2019 Topic Embedded Products B.V. (http://www.topic.nl).
 * All rights reserved.
 */

/*
 * This program writes frames to the DMA controller to fake a camera
 */
#include "dyplo/hardware.hpp"
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <string>
#include <iostream>

static void usage(const char* name)
{
	std::cerr << "usage: " << name << " [-d destination] [-v] [-s] [-w width] [-h height] [-b bpp]\n"
		" -d    Destination DMA index default: 0\n"
		" -s    Streaming DMA mode (only if frame size less than 4MB)\n"
		" -w    Frame width in pixels, default: 1920\n"
		" -h    Frame height in lines, default: 1080\n"
		" -b    Bits per pixel, default: 32\n"
		" -v    Verbose mode, output stats\n"
		"\n";
}

int main(int argc, char** argv)
{
	int main_result = 0;

	try
	{
		int dma_index = 0;
		bool verbose = false;
		unsigned int video_width = 1920;
		unsigned int video_height = 1080;
		unsigned int video_bytes_per_pixel = 4; /* RGBX */

		for (;;)
		{
			int c = getopt(argc, argv, "b:c:d:f:h:k:svw:");
			if (c < 0)
				break;
			switch (c)
			{
			case 'b':
				video_bytes_per_pixel = strtoll(optarg, NULL, 0) / 8;
				break;
			case 'd':
				dma_index = strtoll(optarg, NULL, 0);
				break;
			case 'h':
				video_height = strtoll(optarg, NULL, 0);
				break;
			case 'v':
				verbose = true;
				break;
			case 'w':
				video_width = strtoll(optarg, NULL, 0);
				break;
			case '?':
				usage(argv[0]);
				return 1;
			}
		}

		const unsigned int video_size_bytes = video_width * video_height * video_bytes_per_pixel;

		// Create objects for hardware control
		dyplo::HardwareContext hardware;
		dyplo::HardwareControl hwControl(hardware);

		/* Open the DMA channel */
		dyplo::HardwareDMAFifo camera(hardware.openDMA(dma_index, O_RDWR));

		/* Allocate zero-copy buffers */
		static const unsigned int num_blocks = 8;
		camera.reconfigure(dyplo::HardwareDMAFifo::MODE_COHERENT,
				video_size_bytes, num_blocks, false);

		/* Create reference images */
		for (unsigned int i = 0; i < num_blocks; ++i)
		{
			dyplo::HardwareDMAFifo::Block *block = camera.dequeue();
			block->bytes_used = video_size_bytes;
			block->user_signal = i;
			unsigned char *fb = (unsigned char *)block->data;
			for (unsigned int h = 0; h < video_height; ++h) {
				for (unsigned int w = 0; w < video_width; ++w) {
					fb[0] = h & 0xff;
					fb[1] = ((h + w) >> 4) & 0xff;
					fb[2] = w & 0xff;
					if (video_bytes_per_pixel > 3)
						fb[3] = 0;
					fb += video_bytes_per_pixel;
				}
			}
			for (unsigned int h = 0; h < 32; ++h) {
				fb = (unsigned char *)block->data + ((i * 32) + (h * video_width)) * video_bytes_per_pixel ;
				for (unsigned int w = 0; w < 32; ++w) {
					fb[0] = 0xff;
					fb[1] = 0xff;
					fb[2] = 0xff;
					fb += video_bytes_per_pixel;
				}
			}
			camera.enqueue(block);
		}

		for (;;)
		{
			dyplo::HardwareDMAFifo::Block *block = camera.dequeue();
			block->bytes_used = video_size_bytes;
			camera.enqueue(block);
			if (verbose) std::cerr << '.' << std::flush;
		}
	}
	catch (const std::exception& ex)
	{
		std::cerr << "ERROR:\n" << ex.what() << std::endl;
		main_result = 1;
	}

	return main_result;
}
