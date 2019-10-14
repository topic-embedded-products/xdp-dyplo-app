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

#define DYPLO_NODE_CAMERA_0	0
//#define DYPLO_NODE_CAMERA_1	3

static void usage(const char* name)
{
	std::cerr << "usage: " << name << " [-c #] [-d destination|-f file] [-v] [-s] [-w width] [-h height] [-b bpp] [-k skip]\n"
		" -c    Camera node index, default: 0\n"
		" -d    Destination framebuffer (mmapped), default: /dev/fb0\n"
		" -f    Output to file instead of memory mapped, - for stdout\n"
		" -s    Streaming DMA mode (only if frame size less than 4MB)\n"
		" -w    Frame width in pixels, default: 1920\n"
		" -h    Frame height in lines, default: 1080\n"
		" -b    Bits per pixel, default: 32\n"
		" -v    Verbose mode, output stats\n"
		" -k    Skip frames\n"
		"\n";
}


#include <time.h>

class Stopwatch
{
public:
        struct timespec m_start;
        struct timespec m_stop;

        Stopwatch()
        {
                clock_gettime(CLOCK_MONOTONIC, &m_start);
        }

        void start()
        {
                clock_gettime(CLOCK_MONOTONIC, &m_start);
        }

        void stop()
        {
                clock_gettime(CLOCK_MONOTONIC, &m_stop);
        }

        unsigned int elapsed_us()
        {
                return
                        ((m_stop.tv_sec - m_start.tv_sec) * 1000000) +
                                (m_stop.tv_nsec - m_start.tv_nsec) / 1000;
        }
};

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
		int skip_frames = 7;


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

		int framebuffer_fd;
		if (mmap_framebuffer)
			framebuffer_fd = ::open(fb_name, O_RDWR);
		else
			if (!strcmp(fb_name, "-"))
				framebuffer_fd = 1; /* Causes stdout to be closed later, but we don't care */
			else
				framebuffer_fd = ::open(fb_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
		dyplo::HardwareDMAFifo from_camera(hardware.openDMA(0, O_RDONLY));
		from_camera.addRouteFrom(camera_node);

		/* Allocate buffers, because of the zero-copy system, the driver
		 * will allocate them for us in DMA capable memory, and give us
		 * direct access through a memory map. The library does all the
		 * work for us. */
		static const unsigned int num_blocks = 8;
		from_camera.reconfigure(
			streaming ? dyplo::HardwareDMAFifo::MODE_STREAMING : dyplo::HardwareDMAFifo::MODE_COHERENT,
			video_size_bytes, num_blocks, true);

		/* Prime the reader with empty blocks. Just dequeue all blocks
		 * and enqueue them. */
		for (unsigned int i = 0; i < num_blocks; ++i)
		{
			dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
			block->bytes_used = video_size_bytes;
			from_camera.enqueue(block);
		}

		/* Non-blocking IO */
		// from_camera.fcntl_set_flag(O_NONBLOCK);
		
		Stopwatch s;

		for (;;)
		{
			for (int i = 0; i < skip_frames; ++i)
			{
				dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
				++frames_captured;
				if (block->bytes_used != video_size_bytes)
					++frames_incomplete;
				block->bytes_used = video_size_bytes;
				from_camera.enqueue(block);
			}

			{
				dyplo::HardwareDMAFifo::Block *block;
				block = from_camera.dequeue();
				++frames_captured;
				if (block->bytes_used == video_size_bytes)
				{
					++frames_sent;
					s.start();
					if (fb)
						memcpy(fb, block->data, video_size_bytes);
					else
						framebuffer.write(block->data, video_size_bytes);
					s.stop();
					if (verbose)
						std::cerr << "memcpy: " << s.elapsed_us() << '\n';
				}
				else
					++frames_incomplete;
				block->bytes_used = video_size_bytes;
				from_camera.enqueue(block);
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
