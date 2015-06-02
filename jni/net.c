/* net.c -- transmit controls over udp
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

#include <glib.h>
#include <gio/gio.h>

#include "net.h"
#include "control.h"

static GSocket *socket = NULL;

static gboolean send_controls(gpointer unused, gboolean ignored)
{
    char buf[12];
    GError *err = NULL;

    if(socket == NULL) return FALSE;

    control_get_packet(buf);
    g_socket_send(socket, buf, 12, NULL, &err);
    return TRUE;
}

void net_start(void)
{
    GInetAddress *udpAddress;
    GSocketAddress *udpSocketAddress;
    GError *err = NULL;

    socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &err);
    g_assert(err == NULL);

    udpAddress = g_inet_address_new_from_string("10.24.42.1");
    udpSocketAddress = g_inet_socket_address_new(udpAddress, 5005);

    g_socket_connect(socket, udpSocketAddress, NULL, &err);
    g_assert(err == NULL);

    g_timeout_add(100, (GSourceFunc)send_controls, NULL);
}

void net_stop(void)
{
    socket = NULL;
    g_object_unref(socket);
}
