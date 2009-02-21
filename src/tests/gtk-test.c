/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#pragma GCC diagnostic ignored "-Wstrict-prototypes"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <glib.h>

#include <pulse/context.h>
#include <pulse/glib-mainloop.h>

int main(int argc, char *argv[]) {

    pa_context *c;
    pa_glib_mainloop *m;
    GtkWidget *window;
    int r;

    gtk_init(&argc, &argv);

    g_set_application_name("This is a test");
    gtk_window_set_default_icon_name("foobar");
    g_setenv("PULSE_PROP_media.role", "phone", TRUE);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW (window), g_get_application_name());
    gtk_widget_show_all(window);

    m = pa_glib_mainloop_new(NULL);
    g_assert(m);

    c = pa_context_new(pa_glib_mainloop_get_api(m), NULL);
    g_assert(c);

    r = pa_context_connect(c, NULL, 0, NULL);
    g_assert(r == 0);

    gtk_main();

    pa_context_unref(c);
    pa_glib_mainloop_free(m);

    return 0;
}
