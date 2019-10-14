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

static const unsigned int video_size_pixels = 1920 * 1080;
static const unsigned int video_bytes_per_pixel = 4; /* RGBX */
static const unsigned int video_size_bytes = video_size_pixels * video_bytes_per_pixel;

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
		dyplo::File framebuffer(::open("/dev/fb0", O_RDWR));
		void *fb = mmap_file(framebuffer.handle, PROT_READ | PROT_WRITE, 0, video_size_bytes);

		// Create objects for hardware control
		dyplo::HardwareContext hardware;
		dyplo::HardwareControl hwControl(hardware);

		/* Open the DMA channel */
		dyplo::HardwareDMAFifo from_camera(hardware.openDMA(0, O_RDONLY));
		from_camera.addRouteFrom(DYPLO_NODE_CAMERA_0);

		/* Allocate buffers, because of the zero-copy system, the driver
		 * will allocate them for us in DMA capable memory, and give us
		 * direct access through a memory map. The library does all the
		 * work for us. */
		static const unsigned int num_blocks = 8;
		from_camera.reconfigure(dyplo::HardwareDMAFifo::MODE_COHERENT, video_size_bytes, num_blocks, true);

		/* Prime the reader with empty blocks. Just dequeue all blocks
		 * and enqueue them. */
		for (unsigned int i = 0; i < num_blocks; ++i)
		{
			dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
			block->bytes_used = video_size_bytes;
			from_camera.enqueue(block);
		}

		/* Non-blocking IO */
		from_camera.fcntl_set_flag(O_NONBLOCK);

		for (;;)
		{
			dyplo::HardwareDMAFifo::Block *block = NULL;
			/* Fetch the frame */
			for(;;)
			{
				dyplo::HardwareDMAFifo::Block *nextblock = from_camera.dequeue();
				if (nextblock == NULL)
				{
					//std::cerr << "dequeue NULL\n";
					/* Would block */
					if (block)
						break; /* Got a frame */
					/* No frame, must wait */
					std::cerr << "Frames: " << frames_captured <<
						" Invalid: " << frames_incomplete <<
						" Sent: " << frames_sent <<
						std::endl;
					from_camera.poll_for_incoming_data(1);
					continue;
				}
				//std::cerr << "dequeue " << nextblock->id << '\n';

				if (block)
				{
					//std::cerr << "enqueue o " << block->id << '\n';
					block->bytes_used = video_size_bytes;
					from_camera.enqueue(block);
					block = NULL;
				}

				++frames_captured;
				if (nextblock->bytes_used != video_size_bytes)
				{
					//std::cerr << "enqueue i " << nextblock->id << '\n';
					nextblock->bytes_used = video_size_bytes;
					from_camera.enqueue(nextblock);
					++frames_incomplete;
					continue;
				}

				/* Candidate arrived: A full frame, see if there are more... */
				block = nextblock;
			}

			++frames_sent;
			memcpy(fb, block->data, video_size_bytes);

			//std::cerr << "enqueue s " << block->id << '\n';
			block->bytes_used = video_size_bytes;
			from_camera.enqueue(block);
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
