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
#include <sys/select.h>
#include <unistd.h>
#include <string>
#include <iostream>

#define DYPLO_NODE_CAMERA_0	2
#define DYPLO_NODE_CAMERA_1	3

static const unsigned int video_size_pixels = 720 * 480;
static const unsigned int video_bytes_per_pixel = 3; /* RGB */
static const unsigned int video_size_bytes = video_size_pixels * video_bytes_per_pixel;

static void fcntl_set_flag(int handle, long flag)
{
	int flags = ::fcntl(handle, F_GETFL, 0);
	if (flags < 0)
		throw dyplo::IOException();
	if (::fcntl(handle, F_SETFL, flags | flag) < 0)
		throw dyplo::IOException();
}

int main(int argc, char** argv)
{
	try
	{
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
		static const unsigned int num_blocks = 3;
		from_camera.reconfigure(dyplo::HardwareDMAFifo::MODE_COHERENT, video_size_bytes, num_blocks, true);

		/* Prime the reader with empty blocks. Just dequeue all blocks
		 * and enqueue them. */
		for (unsigned int i = 0; i < num_blocks; ++i)
		{
			dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
			block->bytes_used = video_size_bytes;
			from_camera.enqueue(block);
		}

		dyplo::HardwareDMAFifo::Block *current_frame = NULL;
		unsigned int current_frame_bytes_left = 0;
		const char *current_frame_data = NULL;
		fd_set rfds;
		fd_set wfds;
		fd_set *active_wfds;
		int nfds;
		int output_fd = 1; /* stdout */

		unsigned int frames_captured = 0;
		unsigned int frames_sent = 0;
		unsigned int frames_dropped = 0;
		unsigned int frames_incomplete = 0;

		/* Activate non-blocking IO on the output */
		fcntl_set_flag(output_fd, O_NONBLOCK);

		for (;;)
		{
			FD_ZERO(&rfds);
			FD_SET(from_camera.handle, &rfds);
			nfds = from_camera.handle + 1;
			if (current_frame)
			{
				/* We're busy sending stuff to output */
				FD_ZERO(&wfds);
				FD_SET(output_fd, &wfds);
				if (output_fd >= nfds)
					nfds = output_fd + 1;
				active_wfds = &wfds;
			}
			else
			{
				active_wfds = NULL;
			}

			int ret = select(nfds, &rfds, active_wfds, NULL, NULL);
			if (ret < 0)
				throw dyplo::IOException("select()");

			if (FD_ISSET(from_camera.handle, &rfds))
			{
				/* Fetch the frame */
				dyplo::HardwareDMAFifo::Block *block = from_camera.dequeue();
				++frames_captured;
				/* Drop frame if sending or it it's incomplete */
				if (current_frame || (block->bytes_used != video_size_bytes))
				{
					block->bytes_used = video_size_bytes;
					from_camera.enqueue(block);
					if (current_frame)
						++frames_dropped;
					else
						++frames_incomplete;
				}
				else
				{
					current_frame = block;
					current_frame_data = (const char*)current_frame->data;
					current_frame_bytes_left = current_frame->bytes_used;
				}
			}

			if (active_wfds && FD_ISSET(output_fd, active_wfds))
			{
				if (!current_frame)
					continue; /* Just to be safe */

				ssize_t r = ::write(output_fd, current_frame_data, current_frame_bytes_left);
				if (r <= 0)
				{
					if (r == 0)
						break; /* EOF */
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						continue; /* Try again later */
					else
						throw dyplo::IOException("output");
				}
				else
				{
					current_frame_bytes_left -= r;
					current_frame_data += r;
				}

				if (current_frame_bytes_left == 0)
				{
					++frames_sent;
					current_frame->bytes_used = video_size_bytes;
					from_camera.enqueue(current_frame);
					current_frame = NULL;
				}
			}
		}

		std::cerr << "Frames: " << frames_captured <<
			" Dropped: " << frames_dropped <<
			" Invalid: " << frames_incomplete <<
			" Sent: " << frames_sent <<
			std::endl;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "ERROR:\n" << ex.what() << std::endl;
		return 1;
	}

	return 0;
}
