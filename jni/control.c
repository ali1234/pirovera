/* control.c -- control packet buffer
 *
 * Copyright (C) 2015 Alistair Buxton <a.j.buxton@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>

#include <glib.h>

#include "control.h"

signed short motors[4] = {0,0,0,0};
unsigned short lights = 0;
unsigned short flags = 0;

static GMutex control_mutex;

void control_set_motors(signed short *m)
{
    g_mutex_lock (&control_mutex);

    motors[0] = m[0];
    motors[1] = m[1];
    motors[2] = m[2];
    motors[3] = m[3];

    g_mutex_unlock (&control_mutex);
}

void control_set_lights(signed short l)
{
    g_mutex_lock (&control_mutex);

    lights = l;

    g_mutex_unlock (&control_mutex);
}

void control_set_headlights(gboolean on)
{
    g_mutex_lock (&control_mutex);

    if (on)
        lights |= 1;
    else
        lights &= ~1;

    g_mutex_unlock (&control_mutex);
}

void control_set_taillights(gboolean on)
{
    g_mutex_lock (&control_mutex);

    if (on)
        lights |= 2;
    else
        lights &= ~2;

    g_mutex_unlock (&control_mutex);
}

void control_set_hazardlights(gboolean on)
{
    g_mutex_lock (&control_mutex);

    if (on)
        lights |= 4;
    else
        lights &= ~4;

    g_mutex_unlock (&control_mutex);
}

void control_set_flags(signed short f)
{
    g_mutex_lock (&control_mutex);

    flags = f;

    g_mutex_unlock (&control_mutex);
}

void control_set_left(signed short f)
{
    g_mutex_lock (&control_mutex);

    motors[1] = motors[3] = f;

    g_mutex_unlock (&control_mutex);
}

void control_set_right(signed short f)
{
    g_mutex_lock (&control_mutex);

    motors[0] = motors[2] = f;

    g_mutex_unlock (&control_mutex);
}


void control_get_packet(char *buf)
{
    g_mutex_lock (&control_mutex);

    buf[0] = motors[0] >> 8;
    buf[1] = motors[0] & 0xff;
    buf[2] = motors[1] >> 8;
    buf[3] = motors[1] & 0xff;
    buf[4] = motors[2] >> 8;
    buf[5] = motors[2] & 0xff;
    buf[6] = motors[3] >> 8;
    buf[7] = motors[3] & 0xff;

    buf[8] = lights >> 8;
    buf[9] = lights & 0xff;

    buf[10] = flags >> 8;
    buf[11] = flags & 0xff;

    g_mutex_unlock (&control_mutex);
}


