/*
 * Dyplo example video application for XDP
 *
 * (C) Copyright 2019 Topic Embedded Products B.V. (http://www.topic.nl).
 * All rights reserved.
 */
#pragma once

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
