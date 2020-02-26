/*
 * Dyplo example video application for XDP
 *
 * (C) Copyright 2019 Topic Embedded Products B.V. (http://www.topic.nl).
 * All rights reserved.
 */

/*
 * This program reads video frames from the IO nodes and passes them on while
 * ensuring that only full frames get output. It skips frames if the reader
 * isn't keeping up.
 */
#include "dyplo/hardware.hpp"
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <string>
#include <iostream>
#include <deque>

#include "stopwatch.hpp"

#define DYPLO_NODE_CAMERA_0	0
//#define DYPLO_NODE_CAMERA_1	3

static void usage(const char* name)
{
	std::cerr << "usage: " << name << " [-c #] [-d destination|-f file] [-v] [-s] [-w width] [-h height] [-b bpp] [-k skip]\n"
		" -c    Camera node index, default: 0\n"
		" -d    Destination framebuffer (mmapped), default: /dev/fb0\n"
		" -f    Output to file instead of memory mapped, - for stdout\n"
		" -s    Streaming DMA mode (much faster on MPSoC)\n"
		" -S    Force to use coherent DMA mode\n"
		" -w    Frame width in pixels, default: 1920\n"
		" -h    Frame height in lines, default: 1080\n"
		" -b    Bits per pixel, default: 32\n"
		" -v    Verbose mode, output stats\n"
		" -k    Skip frames before capturing a next frame, default: 0\n"
		"\n";
}

static inline void fcntl_set_flag(int handle, long flag)
{
	int flags = ::fcntl(handle, F_GETFL, 0);
	if (flags < 0)
		throw dyplo::IOException();
	if (::fcntl(handle, F_SETFL, flags | flag) < 0)
		throw dyplo::IOException();
}

static void* mmap_file(int handle, int prot, off_t offset, size_t size)
{
	void* map = ::mmap(NULL, size, prot, MAP_SHARED, handle, offset);
	if (map == MAP_FAILED)
		throw dyplo::IOException("mmap");
	return map;
}

int main(int argc, char** argv)
{
	unsigned int frames_captured = 0;
	unsigned int frames_sent = 0;
	unsigned int frames_dropped = 0;
	unsigned int frames_incomplete = 0;
	int main_result = 0;

	try
	{
		const char *fb_name = "/dev/fb0";
		int camera_node = DYPLO_NODE_CAMERA_0;
		bool verbose = false;
		bool streaming = false;
		bool mmap_framebuffer = true;
		unsigned int video_width = 1920;
		unsigned int video_height = 1080;
		unsigned int video_bytes_per_pixel = 4; /* RGBX */
		int skip_frames = 0;

		for (;;)
		{
			int c = getopt(argc, argv, "b:c:d:f:h:k:sSvw:");
			if (c < 0)
				break;
			switch (c)
			{
			case 'b':
				video_bytes_per_pixel = strtoll(optarg, NULL, 0) / 8;
				break;
			case 'c':
				camera_node = strtoll(optarg, NULL, 0);
				break;
			case 'd':
				mmap_framebuffer = true;
				fb_name = optarg;
				break;
			case 'f':
				mmap_framebuffer = false;
				fb_name = optarg;
				break;
			case 'h':
				video_height = strtoll(optarg, NULL, 0);
				break;
			case 'k':
				skip_frames = strtoll(optarg, NULL, 0);
				break;
			case 's':
				streaming = true;
				break;
			case 'S':
				streaming = false;
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
		unsigned int block_size_bytes = video_size_bytes;

		int framebuffer_fd;
		if (mmap_framebuffer)
			framebuffer_fd = ::open(fb_name, O_RDWR);
		else
			if (!strcmp(fb_name, "-"))
				framebuffer_fd = 1; /* Causes stdout to be closed later, but we don't care */
			else
				framebuffer_fd = ::open(fb_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (framebuffer_fd == -1)
			throw dyplo::IOException(fb_name);
		dyplo::File framebuffer(framebuffer_fd);
		
		void *fb;
		if (mmap_framebuffer)
			fb = mmap_file(framebuffer.handle, PROT_READ | PROT_WRITE, 0, video_size_bytes);
		else
			fb = NULL;

		// Create objects for hardware control
		dyplo::HardwareContext hardware;
		dyplo::HardwareControl hwControl(hardware);

		/* Open the DMA channel */
		dyplo::HardwareDMAFifo from_camera(hardware.openAvailableDMA(O_RDONLY));
		from_camera.addRouteFrom(camera_node);

		/* Allocate buffers, because of the zero-copy system, the driver
		 * will allocate them for us in DMA capable memory, and give us
		 * direct access through a memory map. The library does all the
		 * work for us. */
		static const unsigned int num_blocks = 6;
		/* Streaming mode can only handle 4M per frame, so split into
		 * multiple smaller blocks if the frame size is larger */
		unsigned int blocks_per_frame = 1;
		if (streaming) {
			blocks_per_frame = 1 + (video_size_bytes >> 22);
			block_size_bytes = video_size_bytes / blocks_per_frame;
			from_camera.reconfigure(
				dyplo::HardwareDMAFifo::MODE_STREAMING,
				block_size_bytes, num_blocks, true);
		} else {
			from_camera.reconfigure(
				dyplo::HardwareDMAFifo::MODE_COHERENT,
				block_size_bytes, num_blocks, true);
		}

		/* Prime the reader with empty blocks. Just dequeue all blocks
		 * and enqueue them. */
		for (unsigned int i = 0; i < num_blocks; ++i)
		{
			dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
			block->bytes_used = block_size_bytes;
			from_camera.enqueue(block);
		}

		/* Non-blocking IO */
		// from_camera.fcntl_set_flag(O_NONBLOCK);
		if (verbose)
			std::cerr << "Block: " << blocks_per_frame << " x " << block_size_bytes << std::endl;

		Stopwatch s;

		for (;;)
		{
			/* Throw away blocks.
			 * TODO: maybe (skip_frames * blocks_per_frame)? */
			for (int i = 0; i < skip_frames; ++i)
			{
				dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
				++frames_captured;
				if (verbose)
					std::cerr << "<- skip @" << block->offset << ' ' << block->user_signal << ": " << block->bytes_used << '\n';
				if (block->bytes_used != block_size_bytes)
					++frames_incomplete;
				block->bytes_used = block_size_bytes;
				if (verbose)
					std::cerr << "-> enqA @" << block->offset << '\n';
				from_camera.enqueue(block);
			}

			/* Assemble a full frame, may be multiple blocks */
			{
				std::deque<dyplo::HardwareDMAFifo::Block *> blocks;
				for (;;)
				{
					dyplo::HardwareDMAFifo::Block *block;
					block = from_camera.dequeue();
					if (verbose)
						std::cerr << "<- DEQU @" << block->offset << ' ' << block->user_signal << ": " << block->bytes_used  << '\n';
					blocks.push_back(block);
					++frames_captured;
					/* Incomplete block? Throw everything away */
					if (block->bytes_used != block_size_bytes) {
						if (verbose)
							std::cerr << "incomplete\n";
						while (!blocks.empty()) {
							dyplo::HardwareDMAFifo::Block *b = blocks.front();
							b->bytes_used = block_size_bytes;
							if (verbose)
								std::cerr << "-> enqB @" << b->offset << '\n';
							from_camera.enqueue(b);
							blocks.pop_front();
						}
						continue; /* Try again */
					}
					if (verbose) std::cerr << "framing " << blocks.size() << '\n';
					/* throw away blocks from a different frame */
					uint16_t frame_id = block->user_signal;
					while (blocks.front()->user_signal != frame_id) {
							dyplo::HardwareDMAFifo::Block *b = blocks.front();
							if (verbose)
								std::cerr << "drop " << b->user_signal << '\n';
							b->bytes_used = block_size_bytes;
							if (verbose)
								std::cerr << "-> enqC @" << b->offset << '\n';
							from_camera.enqueue(b);
							blocks.pop_front();
					}
					/* See if we've assembled enough blocks for a frame */
					if (blocks.size() == blocks_per_frame) {
						off_t offset = 0;
						while (!blocks.empty()) {
							dyplo::HardwareDMAFifo::Block *b = blocks.front();
							++frames_sent;
							if (verbose)
								std::cerr << "send @" << offset << " id=" << b->user_signal << '\n';
							s.start();
							if (fb) {
								memcpy((char *)fb + offset, b->data, block_size_bytes);
								offset += block_size_bytes;
							} else
								framebuffer.write(b->data, block_size_bytes);
							s.stop();
							if (verbose)
								std::cerr << "memcpy: " << s.elapsed_us() << '\n';
							b->bytes_used = block_size_bytes;
							if (verbose)
								std::cerr << "-> enqD @" << b->offset << '\n';
							from_camera.enqueue(b);
							blocks.pop_front();
						}
						break; /* Done! */
					}
				}
				/* Flush any remaining blocks */
				while (!blocks.empty()) {
					dyplo::HardwareDMAFifo::Block *b = blocks.front();
					b->bytes_used = block_size_bytes;
					if (verbose)
						std::cerr << "-> enqE @" << b->offset << '\n';
					from_camera.enqueue(b);
					blocks.pop_front();
				}
			}
		}
	}
	catch (const std::exception& ex)
	{
		std::cerr << "ERROR:\n" << ex.what() << std::endl;
		main_result = 1;
	}

	std::cerr << "Frames: " << frames_captured <<
		" Dropped: " << frames_dropped <<
		" Invalid: " << frames_incomplete <<
		" Sent: " << frames_sent <<
		std::endl;

	return main_result;
}
