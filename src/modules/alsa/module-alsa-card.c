/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/modargs.h>
#include <pulsecore/queue.h>

#include <modules/reserve-wrap.h>

#ifdef HAVE_UDEV
#include <modules/udev-util.h>
#endif

#include "alsa-util.h"
#include "alsa-ucm.h"
#include "alsa-sink.h"
#include "alsa-source.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("ALSA Card");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "name=<name for the card/sink/source, to be prefixed> "
        "card_name=<name for the card> "
        "card_properties=<properties for the card> "
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> "
        "source_name=<name for the source> "
        "source_properties=<properties for the source> "
        "namereg_fail=<when false attempt to synthesise new names if they are already taken> "
        "device_id=<ALSA card index> "
        "format=<sample format> "
        "rate=<sample rate> "
        "fragments=<number of fragments> "
        "fragment_size=<fragment size> "
        "mmap=<enable memory mapping?> "
        "tsched=<enable system timer based scheduling mode?> "
        "tsched_buffer_size=<buffer size when using timer based scheduling> "
        "tsched_buffer_watermark=<lower fill watermark> "
        "profile=<profile name> "
        "fixed_latency_range=<disable latency range changes on underrun?> "
        "ignore_dB=<ignore dB information from the device?> "
        "deferred_volume=<Synchronize software and hardware volume changes to avoid momentary jumps?> "
        "profile_set=<profile set configuration file> "
        "paths_dir=<directory containing the path configuration files> "
        "use_ucm=<load use case manager> "
        "avoid_resampling=<use stream original sample rate if possible?> "
        "control=<name of mixer control> "
);

static const char* const valid_modargs[] = {
    "name",
    "card_name",
    "card_properties",
    "sink_name",
    "sink_properties",
    "source_name",
    "source_properties",
    "namereg_fail",
    "device_id",
    "format",
    "rate",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    "fixed_latency_range",
    "profile",
    "ignore_dB",
    "deferred_volume",
    "profile_set",
    "paths_dir",
    "use_ucm",
    "avoid_resampling",
    "control",
    NULL
};

#define DEFAULT_DEVICE_ID "0"

#define PULSE_MODARGS "PULSE_MODARGS"

/* dynamic profile priority bonus, for all alsa profiles, the original priority
   needs to be less than 0x7fff (32767), then could apply the rule of priority
   bonus. So far there are 2 kinds of alsa profiles, one is from alsa ucm, the
   other is from mixer profile-sets, their priorities are all far less than 0x7fff
*/
#define PROFILE_PRIO_BONUS 0x8000

struct userdata {
    pa_core *core;
    pa_module *module;

    char *device_id;
    int alsa_card_index;

    pa_hashmap *mixers;
    pa_hashmap *jacks;

    pa_card *card;

    pa_modargs *modargs;

    pa_alsa_profile_set *profile_set;

    /* ucm stuffs */
    bool use_ucm;
    pa_alsa_ucm_config ucm;

};

struct profile_data {
    pa_alsa_profile *profile;
};

static void add_profiles(struct userdata *u, pa_hashmap *h, pa_hashmap *ports) {
    pa_alsa_profile *ap;
    void *state;

    pa_assert(u);
    pa_assert(h);

    PA_HASHMAP_FOREACH(ap, u->profile_set->profiles, state) {
        struct profile_data *d;
        pa_card_profile *cp;
        pa_alsa_mapping *m;
        uint32_t idx;

        cp = pa_card_profile_new(ap->name, ap->description, sizeof(struct profile_data));
        cp->priority = ap->priority ? ap->priority : 1;
        cp->input_name = pa_xstrdup(ap->input_name);
        cp->output_name = pa_xstrdup(ap->output_name);

        if (ap->output_mappings) {
            cp->n_sinks = pa_idxset_size(ap->output_mappings);

            PA_IDXSET_FOREACH(m, ap->output_mappings, idx) {
                if (u->use_ucm)
                    pa_alsa_ucm_add_port(NULL, &m->ucm_context, true, ports, cp, u->core);
                else
                    pa_alsa_path_set_add_ports(m->output_path_set, cp, ports, NULL, u->core);
                if (m->channel_map.channels > cp->max_sink_channels)
                    cp->max_sink_channels = m->channel_map.channels;
            }
        }

        if (ap->input_mappings) {
            cp->n_sources = pa_idxset_size(ap->input_mappings);

            PA_IDXSET_FOREACH(m, ap->input_mappings, idx) {
                if (u->use_ucm)
                    pa_alsa_ucm_add_port(NULL, &m->ucm_context, false, ports, cp, u->core);
                else
                    pa_alsa_path_set_add_ports(m->input_path_set, cp, ports, NULL, u->core);
                if (m->channel_map.channels > cp->max_source_channels)
                    cp->max_source_channels = m->channel_map.channels;
            }
        }

        d = PA_CARD_PROFILE_DATA(cp);
        d->profile = ap;

        pa_hashmap_put(h, cp->name, cp);
    }
}

static void add_disabled_profile(pa_hashmap *profiles) {
    pa_card_profile *p;
    struct profile_data *d;

    p = pa_card_profile_new("off", _("Off"), sizeof(struct profile_data));

    d = PA_CARD_PROFILE_DATA(p);
    d->profile = NULL;

    pa_hashmap_put(profiles, p->name, p);
}

static int card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    struct userdata *u;
    struct profile_data *nd, *od;
    uint32_t idx;
    pa_alsa_mapping *am;
    pa_queue *sink_inputs = NULL, *source_outputs = NULL;
    int ret = 0;

    pa_assert(c);
    pa_assert(new_profile);
    pa_assert_se(u = c->userdata);

    nd = PA_CARD_PROFILE_DATA(new_profile);
    od = PA_CARD_PROFILE_DATA(c->active_profile);

    if (od->profile && od->profile->output_mappings)
        PA_IDXSET_FOREACH(am, od->profile->output_mappings, idx) {
            if (!am->sink)
                continue;

            if (nd->profile &&
                nd->profile->output_mappings &&
                pa_idxset_get_by_data(nd->profile->output_mappings, am, NULL))
                continue;

            sink_inputs = pa_sink_move_all_start(am->sink, sink_inputs);
            pa_alsa_sink_free(am->sink);
            am->sink = NULL;
        }

    if (od->profile && od->profile->input_mappings)
        PA_IDXSET_FOREACH(am, od->profile->input_mappings, idx) {
            if (!am->source)
                continue;

            if (nd->profile &&
                nd->profile->input_mappings &&
                pa_idxset_get_by_data(nd->profile->input_mappings, am, NULL))
                continue;

            source_outputs = pa_source_move_all_start(am->source, source_outputs);
            pa_alsa_source_free(am->source);
            am->source = NULL;
        }

    /* if UCM is available for this card then update the verb */
    if (u->use_ucm) {
        if (pa_alsa_ucm_set_profile(&u->ucm, c, nd->profile, od->profile) < 0) {
            ret = -1;
            goto finish;
        }
    }

    if (nd->profile && nd->profile->output_mappings)
        PA_IDXSET_FOREACH(am, nd->profile->output_mappings, idx) {

            if (!am->sink)
                am->sink = pa_alsa_sink_new(c->module, u->modargs, __FILE__, c, am);

            if (sink_inputs && am->sink) {
                pa_sink_move_all_finish(am->sink, sink_inputs, false);
                sink_inputs = NULL;
            }
        }

    if (nd->profile && nd->profile->input_mappings)
        PA_IDXSET_FOREACH(am, nd->profile->input_mappings, idx) {

            if (!am->source)
                am->source = pa_alsa_source_new(c->module, u->modargs, __FILE__, c, am);

            if (source_outputs && am->source) {
                pa_source_move_all_finish(am->source, source_outputs, false);
                source_outputs = NULL;
            }
        }

finish:
    if (sink_inputs)
        pa_sink_move_all_fail(sink_inputs);

    if (source_outputs)
        pa_source_move_all_fail(source_outputs);

    return ret;
}

static void init_profile(struct userdata *u) {
    uint32_t idx;
    pa_alsa_mapping *am;
    struct profile_data *d;
    pa_alsa_ucm_config *ucm = &u->ucm;

    pa_assert(u);

    d = PA_CARD_PROFILE_DATA(u->card->active_profile);

    if (d->profile && u->use_ucm) {
        /* Set initial verb */
        if (pa_alsa_ucm_set_profile(ucm, u->card, d->profile, NULL) < 0) {
            pa_log("Failed to set ucm profile %s", d->profile->name);
            return;
        }
    }

    if (d->profile && d->profile->output_mappings)
        PA_IDXSET_FOREACH(am, d->profile->output_mappings, idx)
            am->sink = pa_alsa_sink_new(u->module, u->modargs, __FILE__, u->card, am);

    if (d->profile && d->profile->input_mappings)
        PA_IDXSET_FOREACH(am, d->profile->input_mappings, idx)
            am->source = pa_alsa_source_new(u->module, u->modargs, __FILE__, u->card, am);
}

static pa_available_t calc_port_state(pa_device_port *p, struct userdata *u) {
    void *state;
    pa_alsa_jack *jack;
    pa_available_t pa = PA_AVAILABLE_UNKNOWN;
    pa_device_port *port;

    PA_HASHMAP_FOREACH(jack, u->jacks, state) {
        pa_available_t cpa;

        if (u->use_ucm)
            port = pa_hashmap_get(u->card->ports, jack->name);
        else {
            if (jack->path)
                port = jack->path->port;
            else
                continue;
        }

        if (p != port)
            continue;

        cpa = jack->plugged_in ? jack->state_plugged : jack->state_unplugged;

        if (cpa == PA_AVAILABLE_NO) {
          /* If a plugged-in jack causes the availability to go to NO, it
           * should override all other availability information (like a
           * blacklist) so set and bail */
          if (jack->plugged_in) {
            pa = cpa;
            break;
          }

          /* If the current availablility is unknown go the more precise no,
           * but otherwise don't change state */
          if (pa == PA_AVAILABLE_UNKNOWN)
            pa = cpa;
        } else if (cpa == PA_AVAILABLE_YES) {
          /* Output is available through at least one jack, so go to that
           * level of availability. We still need to continue iterating through
           * the jacks in case a jack is plugged in that forces the state to no
           */
          pa = cpa;
        }
    }
    return pa;
}

struct temp_port_avail {
    pa_device_port *port;
    pa_available_t avail;
};

static int report_jack_state(snd_mixer_elem_t *melem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(melem);
    snd_hctl_elem_t *elem = snd_mixer_elem_get_private(melem);
    snd_ctl_elem_value_t *elem_value;
    bool plugged_in;
    void *state;
    pa_alsa_jack *jack;
    struct temp_port_avail *tp, *tports;
    pa_card_profile *profile;
    pa_available_t active_available = PA_AVAILABLE_UNKNOWN;

    pa_assert(u);

    /* Changing the jack state may cause a port change, and a port change will
     * make the sink or source change the mixer settings. If there are multiple
     * users having pulseaudio running, the mixer changes done by inactive
     * users may mess up the volume settings for the active users, because when
     * the inactive users change the mixer settings, those changes are picked
     * up by the active user's pulseaudio instance and the changes are
     * interpreted as if the active user changed the settings manually e.g.
     * with alsamixer. Even single-user systems suffer from this, because gdm
     * runs its own pulseaudio instance.
     *
     * We rerun this function when being unsuspended to catch up on jack state
     * changes */
    if (u->card->suspend_cause & PA_SUSPEND_SESSION)
        return 0;

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    snd_ctl_elem_value_alloca(&elem_value);
    if (snd_hctl_elem_read(elem, elem_value) < 0) {
        pa_log_warn("Failed to read jack detection from '%s'", pa_strnull(snd_hctl_elem_get_name(elem)));
        return 0;
    }

    plugged_in = !!snd_ctl_elem_value_get_boolean(elem_value, 0);

    pa_log_debug("Jack '%s' is now %s", pa_strnull(snd_hctl_elem_get_name(elem)), plugged_in ? "plugged in" : "unplugged");

    tports = tp = pa_xnew0(struct temp_port_avail, pa_hashmap_size(u->jacks)+1);

    PA_HASHMAP_FOREACH(jack, u->jacks, state)
        if (jack->melem == melem) {
            pa_alsa_jack_set_plugged_in(jack, plugged_in);

            if (u->use_ucm) {
                /* When using UCM, pa_alsa_jack_set_plugged_in() maps the jack
                 * state to port availability. */
                continue;
            }

            /* When not using UCM, we have to do the jack state -> port
             * availability mapping ourselves. */
            pa_assert_se(tp->port = jack->path->port);
            tp->avail = calc_port_state(tp->port, u);
            tp++;
        }

    /* Report available ports before unavailable ones: in case port 1 becomes available when port 2 becomes unavailable,
       this prevents an unnecessary switch port 1 -> port 3 -> port 2 */

    for (tp = tports; tp->port; tp++)
        if (tp->avail != PA_AVAILABLE_NO)
           pa_device_port_set_available(tp->port, tp->avail);
    for (tp = tports; tp->port; tp++)
        if (tp->avail == PA_AVAILABLE_NO)
           pa_device_port_set_available(tp->port, tp->avail);

    for (tp = tports; tp->port; tp++) {
        pa_alsa_port_data *data;
        pa_sink *sink;
        uint32_t idx;

        data = PA_DEVICE_PORT_DATA(tp->port);

        if (!data->suspend_when_unavailable)
            continue;

        PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
            if (sink->active_port == tp->port)
                pa_sink_suspend(sink, tp->avail == PA_AVAILABLE_NO, PA_SUSPEND_UNAVAILABLE);
        }
    }

    /* Update profile availabilities. Ideally we would mark all profiles
     * unavailable that contain unavailable devices. We can't currently do that
     * in all cases, because if there are multiple sinks in a profile, and the
     * profile contains a mix of available and unavailable ports, we don't know
     * how the ports are distributed between the different sinks. It's possible
     * that some sinks contain only unavailable ports, in which case we should
     * mark the profile as unavailable, but it's also possible that all sinks
     * contain at least one available port, in which case we should mark the
     * profile as available. Until the data structures are improved so that we
     * can distinguish between these two cases, we mark the problematic cases
     * as available (well, "unknown" to be precise, but there's little
     * practical difference).
     *
     * A profile will be marked unavailable:
     * only contains output ports and all ports are unavailable
     * only contains input ports and all ports are unavailable
     * contains both input and output ports and all ports are unavailable
     *
     * A profile will be awarded priority bonus:
     * only contains output ports and at least one port is available
     * only contains input ports and at least one port is available
     * contains both output and input ports and at least one output port
     * and one input port are available
     *
     * The rest profiles will not be marked unavailable and will not be
     * awarded priority bonus
     *
     * If there are no output ports at all, but the profile contains at least
     * one sink, then the output is considered to be available. */
    if (u->card->active_profile)
        active_available = u->card->active_profile->available;
    PA_HASHMAP_FOREACH(profile, u->card->profiles, state) {
        pa_device_port *port;
        void *state2;
        bool has_input_port = false;
        bool has_output_port = false;
        bool found_available_input_port = false;
        bool found_available_output_port = false;
        pa_available_t available = PA_AVAILABLE_UNKNOWN;

        profile->priority &= ~PROFILE_PRIO_BONUS;
        PA_HASHMAP_FOREACH(port, u->card->ports, state2) {
            if (!pa_hashmap_get(port->profiles, profile->name))
                continue;

            if (port->direction == PA_DIRECTION_INPUT) {
                has_input_port = true;

                if (port->available != PA_AVAILABLE_NO)
                    found_available_input_port = true;
            } else {
                has_output_port = true;

                if (port->available != PA_AVAILABLE_NO)
                    found_available_output_port = true;
            }
        }

        if ((has_input_port && found_available_input_port && !has_output_port) ||
            (has_output_port && found_available_output_port && !has_input_port) ||
            (has_input_port && found_available_input_port && has_output_port && found_available_output_port))
                profile->priority |= PROFILE_PRIO_BONUS;

        if ((has_input_port && !found_available_input_port && has_output_port && !found_available_output_port) ||
            (has_input_port && !found_available_input_port && !has_output_port) ||
            (has_output_port && !found_available_output_port && !has_input_port))
                available = PA_AVAILABLE_NO;

        /* We want to update the active profile's status last, so logic that
         * may change the active profile based on profile availability status
         * has an updated view of all profiles' availabilities. */
        if (profile == u->card->active_profile)
            active_available = available;
        else
            pa_card_profile_set_available(profile, available);
    }

    if (u->card->active_profile)
        pa_card_profile_set_available(u->card->active_profile, active_available);

    pa_xfree(tports);
    return 0;
}

static pa_device_port* find_port_with_eld_device(struct userdata *u, int device) {
    void *state;
    pa_device_port *p;

    if (u->use_ucm) {
        PA_HASHMAP_FOREACH(p, u->card->ports, state) {
            pa_alsa_ucm_port_data *data = PA_DEVICE_PORT_DATA(p);
            pa_assert(data->eld_mixer_device_name);
            if (device == data->eld_device)
                return p;
        }
    } else {
        PA_HASHMAP_FOREACH(p, u->card->ports, state) {
            pa_alsa_port_data *data = PA_DEVICE_PORT_DATA(p);
            pa_assert(data->path);
            if (device == data->path->eld_device)
                return p;
        }
    }
    return NULL;
}

static int hdmi_eld_changed(snd_mixer_elem_t *melem, unsigned int mask) {
    struct userdata *u = snd_mixer_elem_get_callback_private(melem);
    snd_hctl_elem_t *elem = snd_mixer_elem_get_private(melem);
    int device = snd_hctl_elem_get_device(elem);
    const char *old_monitor_name;
    pa_device_port *p;
    pa_hdmi_eld eld;
    bool changed = false;

    if (mask == SND_CTL_EVENT_MASK_REMOVE)
        return 0;

    p = find_port_with_eld_device(u, device);
    if (p == NULL) {
        pa_log_error("Invalid device changed in ALSA: %d", device);
        return 0;
    }

    if (pa_alsa_get_hdmi_eld(elem, &eld) < 0)
        memset(&eld, 0, sizeof(eld));

    old_monitor_name = pa_proplist_gets(p->proplist, PA_PROP_DEVICE_PRODUCT_NAME);
    if (eld.monitor_name[0] == '\0') {
        changed |= old_monitor_name != NULL;
        pa_proplist_unset(p->proplist, PA_PROP_DEVICE_PRODUCT_NAME);
    } else {
        changed |= (old_monitor_name == NULL) || (strcmp(old_monitor_name, eld.monitor_name) != 0);
        pa_proplist_sets(p->proplist, PA_PROP_DEVICE_PRODUCT_NAME, eld.monitor_name);
    }

    if (changed && mask != 0)
        pa_subscription_post(u->core, PA_SUBSCRIPTION_EVENT_CARD|PA_SUBSCRIPTION_EVENT_CHANGE, u->card->index);

    return 0;
}

static void init_eld_ctls(struct userdata *u) {
    void *state;
    pa_device_port *port;

    /* The code in this function expects ports to have a pa_alsa_port_data
     * struct as their data, but in UCM mode ports don't have any data. Hence,
     * the ELD controls can't currently be used in UCM mode. */
    PA_HASHMAP_FOREACH(port, u->card->ports, state) {
        snd_mixer_t *mixer_handle;
        snd_mixer_elem_t* melem;
        int device;

        if (u->use_ucm) {
            pa_alsa_ucm_port_data *data = PA_DEVICE_PORT_DATA(port);
            device = data->eld_device;
            if (device < 0 || !data->eld_mixer_device_name)
                continue;

            mixer_handle = pa_alsa_open_mixer_by_name(u->mixers, data->eld_mixer_device_name, true);
        } else {
            pa_alsa_port_data *data = PA_DEVICE_PORT_DATA(port);

            pa_assert(data->path);

            device = data->path->eld_device;
            if (device < 0)
                continue;

            mixer_handle = pa_alsa_open_mixer(u->mixers, u->alsa_card_index, true);
        }

        if (!mixer_handle)
            continue;

        melem = pa_alsa_mixer_find_pcm(mixer_handle, "ELD", device);
        if (melem) {
            pa_alsa_mixer_set_fdlist(u->mixers, mixer_handle, u->core->mainloop);
            snd_mixer_elem_set_callback(melem, hdmi_eld_changed);
            snd_mixer_elem_set_callback_private(melem, u);
            hdmi_eld_changed(melem, 0);
            pa_log_info("ELD device found for port %s (%d).", port->name, device);
        }
        else
            pa_log_debug("No ELD device found for port %s (%d).", port->name, device);
    }
}

static void init_jacks(struct userdata *u) {
    void *state;
    pa_alsa_path* path;
    pa_alsa_jack* jack;
    char buf[64];

    u->jacks = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    if (u->use_ucm) {
        PA_LLIST_FOREACH(jack, u->ucm.jacks)
            if (jack->has_control)
                pa_hashmap_put(u->jacks, jack, jack);
    } else {
        /* See if we have any jacks */
        if (u->profile_set->output_paths)
            PA_HASHMAP_FOREACH(path, u->profile_set->output_paths, state)
                PA_LLIST_FOREACH(jack, path->jacks)
                    if (jack->has_control)
                        pa_hashmap_put(u->jacks, jack, jack);

        if (u->profile_set->input_paths)
            PA_HASHMAP_FOREACH(path, u->profile_set->input_paths, state)
                PA_LLIST_FOREACH(jack, path->jacks)
                    if (jack->has_control)
                        pa_hashmap_put(u->jacks, jack, jack);
    }

    pa_log_debug("Found %d jacks.", pa_hashmap_size(u->jacks));

    if (pa_hashmap_size(u->jacks) == 0)
        return;

    PA_HASHMAP_FOREACH(jack, u->jacks, state) {
        if (!jack->mixer_device_name) {
            jack->mixer_handle = pa_alsa_open_mixer(u->mixers, u->alsa_card_index, false);
            if (!jack->mixer_handle) {
               pa_log("Failed to open mixer for card %d for jack detection", u->alsa_card_index);
               continue;
            }
        } else {
            jack->mixer_handle = pa_alsa_open_mixer_by_name(u->mixers, jack->mixer_device_name, false);
            if (!jack->mixer_handle) {
               pa_log("Failed to open mixer '%s' for jack detection", jack->mixer_device_name);
              continue;
            }
        }
        pa_alsa_mixer_set_fdlist(u->mixers, jack->mixer_handle, u->core->mainloop);
        jack->melem = pa_alsa_mixer_find_card(jack->mixer_handle, &jack->alsa_id, 0);
        if (!jack->melem) {
            pa_alsa_mixer_id_to_string(buf, sizeof(buf), &jack->alsa_id);
            pa_log_warn("Jack %s seems to have disappeared.", buf);
            pa_alsa_jack_set_has_control(jack, false);
            continue;
        }
        snd_mixer_elem_set_callback(jack->melem, report_jack_state);
        snd_mixer_elem_set_callback_private(jack->melem, u);
        report_jack_state(jack->melem, 0);
    }
}

static void prune_singleton_availability_groups(pa_hashmap *ports) {
    pa_device_port *p;
    pa_hashmap *group_counts;
    void *state, *count;
    const char *group;

    /* Collect groups and erase those that don't have more than 1 path */
    group_counts = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    PA_HASHMAP_FOREACH(p, ports, state) {
        if (p->availability_group) {
            count = pa_hashmap_get(group_counts, p->availability_group);
            pa_hashmap_remove(group_counts, p->availability_group);
            pa_hashmap_put(group_counts, p->availability_group, PA_UINT_TO_PTR(PA_PTR_TO_UINT(count) + 1));
        }
    }

    /* Now we have an availability_group -> count map, let's drop all groups
     * that have only one member */
    PA_HASHMAP_FOREACH_KV(group, count, group_counts, state) {
        if (count == PA_UINT_TO_PTR(1))
            pa_hashmap_remove(group_counts, group);
    }

    PA_HASHMAP_FOREACH(p, ports, state) {
        if (p->availability_group && !pa_hashmap_get(group_counts, p->availability_group)) {
            pa_log_debug("Pruned singleton availability group %s from port %s", p->availability_group, p->name);

            pa_xfree(p->availability_group);
            p->availability_group = NULL;
        }
    }

    pa_hashmap_free(group_counts);
}

static void set_card_name(pa_card_new_data *data, pa_modargs *ma, const char *device_id) {
    char *t;
    const char *n;

    pa_assert(data);
    pa_assert(ma);
    pa_assert(device_id);

    if ((n = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_card_new_data_set_name(data, n);
        data->namereg_fail = true;
        return;
    }

    if ((n = pa_modargs_get_value(ma, "name", NULL)))
        data->namereg_fail = true;
    else {
        n = device_id;
        data->namereg_fail = false;
    }

    t = pa_sprintf_malloc("alsa_card.%s", n);
    pa_card_new_data_set_name(data, t);
    pa_xfree(t);
}

static pa_hook_result_t card_suspend_changed(pa_core *c, pa_card *card, struct userdata *u) {
    void *state;
    pa_alsa_jack *jack;

    if (card->suspend_cause == 0) {
        /* We were unsuspended, update jack state in case it changed while we were suspended */
        PA_HASHMAP_FOREACH(jack, u->jacks, state) {
            if (jack->melem)
                report_jack_state(jack->melem, 0);
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_hook_callback(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *role;
    pa_sink *sink = sink_input->sink;

    pa_assert(sink);

    role = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_ROLE);

    /* new sink input linked to sink of this card */
    if (role && sink->card == u->card)
        pa_alsa_ucm_roled_stream_begin(&u->ucm, role, PA_DIRECTION_OUTPUT);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_put_hook_callback(pa_core *c, pa_source_output *source_output, struct userdata *u) {
    const char *role;
    pa_source *source = source_output->source;

    pa_assert(source);

    role = pa_proplist_gets(source_output->proplist, PA_PROP_MEDIA_ROLE);

    /* new source output linked to source of this card */
    if (role && source->card == u->card)
        pa_alsa_ucm_roled_stream_begin(&u->ucm, role, PA_DIRECTION_INPUT);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink_hook_callback(pa_core *c, pa_sink_input *sink_input, struct userdata *u) {
    const char *role;
    pa_sink *sink = sink_input->sink;

    pa_assert(sink);

    role = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_ROLE);

    /* new sink input unlinked from sink of this card */
    if (role && sink->card == u->card)
        pa_alsa_ucm_roled_stream_end(&u->ucm, role, PA_DIRECTION_OUTPUT);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_output_unlink_hook_callback(pa_core *c, pa_source_output *source_output, struct userdata *u) {
    const char *role;
    pa_source *source = source_output->source;

    pa_assert(source);

    role = pa_proplist_gets(source_output->proplist, PA_PROP_MEDIA_ROLE);

    /* new source output unlinked from source of this card */
    if (role && source->card == u->card)
        pa_alsa_ucm_roled_stream_end(&u->ucm, role, PA_DIRECTION_INPUT);

    return PA_HOOK_OK;
}

int pa__init(pa_module *m) {
    pa_card_new_data data;
    bool ignore_dB = false;
    struct userdata *u;
    pa_reserve_wrapper *reserve = NULL;
    const char *description;
    const char *profile_str = NULL;
    char *fn = NULL;
    char *udev_args = NULL;
    bool namereg_fail = false;
    int err = -PA_MODULE_ERR_UNSPECIFIED, rval;

    pa_alsa_refcnt_inc();

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->use_ucm = true;
    u->ucm.core = m->core;

    u->mixers = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                    pa_xfree, (pa_free_cb_t) pa_alsa_mixer_free);
    u->ucm.mixers = u->mixers; /* alias */

    if (!(u->modargs = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    u->device_id = pa_xstrdup(pa_modargs_get_value(u->modargs, "device_id", DEFAULT_DEVICE_ID));

    if ((u->alsa_card_index = snd_card_get_index(u->device_id)) < 0) {
        pa_log("Card '%s' doesn't exist: %s", u->device_id, pa_alsa_strerror(u->alsa_card_index));
        goto fail;
    }

#ifdef HAVE_UDEV
    udev_args = pa_udev_get_property(u->alsa_card_index, PULSE_MODARGS);
#endif

    if (udev_args) {
        bool udev_modargs_success = true;
        pa_modargs *temp_ma = pa_modargs_new(udev_args, valid_modargs);

        if (temp_ma) {
            /* do not try to replace device_id */

            if (pa_modargs_remove_key(temp_ma, "device_id") == 0) {
                pa_log_warn("Unexpected 'device_id' module argument override ignored from udev " PULSE_MODARGS "='%s'", udev_args);
            }

            /* Implement modargs override by copying original module arguments
             * over udev entry arguments ignoring duplicates. */

            if (pa_modargs_merge_missing(temp_ma, u->modargs, valid_modargs) == 0) {
                /* swap module arguments */
                pa_modargs *old_ma = u->modargs;
                u->modargs = temp_ma;
                temp_ma = old_ma;

                pa_log_info("Applied module arguments override from udev " PULSE_MODARGS "='%s'", udev_args);
            } else {
                pa_log("Failed to apply module arguments override from udev " PULSE_MODARGS "='%s'", udev_args);
                udev_modargs_success = false;
            }

            pa_modargs_free(temp_ma);
        } else {
            pa_log("Failed to parse module arguments from udev " PULSE_MODARGS "='%s'", udev_args);
            udev_modargs_success = false;
        }
        pa_xfree(udev_args);

        if (!udev_modargs_success)
            goto fail;
    }

    if (pa_modargs_get_value_boolean(u->modargs, "ignore_dB", &ignore_dB) < 0) {
        pa_log("Failed to parse ignore_dB argument.");
        goto fail;
    }

    if (!pa_in_system_mode()) {
        char *rname;

        if ((rname = pa_alsa_get_reserve_name(u->device_id))) {
            reserve = pa_reserve_wrapper_get(m->core, rname);
            pa_xfree(rname);

            if (!reserve)
                goto fail;
        }
    }

    if (pa_modargs_get_value_boolean(u->modargs, "use_ucm", &u->use_ucm) < 0) {
        pa_log("Failed to parse use_ucm argument.");
        goto fail;
    }

    /* Force ALSA to reread its configuration. This matters if our device
     * was hot-plugged after ALSA has already read its configuration - see
     * https://bugs.freedesktop.org/show_bug.cgi?id=54029
     */

    snd_config_update_free_global();

    rval = u->use_ucm ? pa_alsa_ucm_query_profiles(&u->ucm, u->alsa_card_index) : -1;
    if (rval == -PA_ALSA_ERR_UCM_LINKED) {
        err = -PA_MODULE_ERR_SKIP;
        goto fail;
    }
    if (rval == 0) {
        pa_log_info("Found UCM profiles");

        u->profile_set = pa_alsa_ucm_add_profile_set(&u->ucm, &u->core->default_channel_map);

        /* hook start of sink input/source output to enable modifiers */
        /* A little bit later than module-role-cork */
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE+10,
                (pa_hook_cb_t) sink_input_put_hook_callback, u);
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT], PA_HOOK_LATE+10,
                (pa_hook_cb_t) source_output_put_hook_callback, u);

        /* hook end of sink input/source output to disable modifiers */
        /* A little bit later than module-role-cork */
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE+10,
                (pa_hook_cb_t) sink_input_unlink_hook_callback, u);
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK], PA_HOOK_LATE+10,
                (pa_hook_cb_t) source_output_unlink_hook_callback, u);
    }
    else {
        u->use_ucm = false;
#ifdef HAVE_UDEV
        fn = pa_udev_get_property(u->alsa_card_index, "PULSE_PROFILE_SET");
#endif

        if (pa_modargs_get_value(u->modargs, "profile_set", NULL)) {
            pa_xfree(fn);
            fn = pa_xstrdup(pa_modargs_get_value(u->modargs, "profile_set", NULL));
        }

        u->profile_set = pa_alsa_profile_set_new(fn, &u->core->default_channel_map);
        pa_xfree(fn);
    }

    if (!u->profile_set)
        goto fail;

    u->profile_set->ignore_dB = ignore_dB;

    pa_alsa_profile_set_probe(u->profile_set, u->mixers, u->device_id, &m->core->default_sample_spec, m->core->default_n_fragments, m->core->default_fragment_size_msec);
    pa_alsa_profile_set_dump(u->profile_set);

    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;

    pa_alsa_init_proplist_card(m->core, data.proplist, u->alsa_card_index);

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->device_id);
    pa_alsa_init_description(data.proplist, NULL);
    set_card_name(&data, u->modargs, u->device_id);

    /* We need to give pa_modargs_get_value_boolean() a pointer to a local
     * variable instead of using &data.namereg_fail directly, because
     * data.namereg_fail is a bitfield and taking the address of a bitfield
     * variable is impossible. */
    namereg_fail = data.namereg_fail;
    if (pa_modargs_get_value_boolean(u->modargs, "namereg_fail", &namereg_fail) < 0) {
        pa_log("Failed to parse namereg_fail argument.");
        pa_card_new_data_done(&data);
        goto fail;
    }
    data.namereg_fail = namereg_fail;

    if (reserve)
        if ((description = pa_proplist_gets(data.proplist, PA_PROP_DEVICE_DESCRIPTION)))
            pa_reserve_wrapper_set_application_device_name(reserve, description);

    add_profiles(u, data.profiles, data.ports);

    if (pa_hashmap_isempty(data.profiles)) {
        pa_log("Failed to find a working profile.");
        pa_card_new_data_done(&data);
        goto fail;
    }

    add_disabled_profile(data.profiles);
    prune_singleton_availability_groups(data.ports);

    if (pa_modargs_get_proplist(u->modargs, "card_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_card_new_data_done(&data);
        goto fail;
    }

    /* The Intel HDMI LPE driver needs some special handling. When the HDMI
     * cable is not plugged in, trying to play audio doesn't work. Any written
     * audio is immediately discarded and an underrun is reported, and that
     * results in an infinite loop of "fill buffer, handle underrun". To work
     * around this issue, the suspend_when_unavailable flag is used to stop
     * playback when the HDMI cable is unplugged. */
    if (!u->use_ucm &&
        pa_safe_streq(pa_proplist_gets(data.proplist, "alsa.driver_name"), "snd_hdmi_lpe_audio")) {
        pa_device_port *port;
        void *state;

        PA_HASHMAP_FOREACH(port, data.ports, state) {
            pa_alsa_port_data *port_data;

            port_data = PA_DEVICE_PORT_DATA(port);
            port_data->suspend_when_unavailable = true;
        }
    }

    u->card = pa_card_new(m->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card)
        goto fail;

    u->card->userdata = u;
    u->card->set_profile = card_set_profile;

    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_SUSPEND_CHANGED], PA_HOOK_NORMAL,
            (pa_hook_cb_t) card_suspend_changed, u);

    init_jacks(u);

    pa_card_choose_initial_profile(u->card);

    /* If the "profile" modarg is given, we have to override whatever the usual
     * policy chose in pa_card_choose_initial_profile(). */
    profile_str = pa_modargs_get_value(u->modargs, "profile", NULL);
    if (profile_str) {
        pa_card_profile *profile;

        profile = pa_hashmap_get(u->card->profiles, profile_str);
        if (!profile) {
            pa_log("No such profile: %s", profile_str);
            goto fail;
        }

        pa_card_set_profile(u->card, profile, false);
    }

    pa_card_put(u->card);

    init_profile(u);
    init_eld_ctls(u);

    /* Remove all probe only mixers */
    if (u->mixers) {
       const char *devname;
       pa_alsa_mixer *pm;
       void *state;
       PA_HASHMAP_FOREACH_KV(devname, pm, u->mixers, state)
           if (pm->used_for_probe_only)
               pa_hashmap_remove_and_free(u->mixers, devname);
    }

    if (reserve)
        pa_reserve_wrapper_unref(reserve);

    if (!pa_hashmap_isempty(u->profile_set->decibel_fixes))
        pa_log_warn("Card %s uses decibel fixes (i.e. overrides the decibel information for some alsa volume elements). "
                    "Please note that this feature is meant just as a help for figuring out the correct decibel values. "
                    "PulseAudio is not the correct place to maintain the decibel mappings! The fixed decibel values "
                    "should be sent to ALSA developers so that they can fix the driver. If it turns out that this feature "
                    "is abused (i.e. fixes are not pushed to ALSA), the decibel fix feature may be removed in some future "
                    "PulseAudio version.", u->card->name);

    return 0;

fail:
    if (reserve)
        pa_reserve_wrapper_unref(reserve);

    pa__done(m);

    return err;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;
    int n = 0;
    uint32_t idx;
    pa_sink *sink;
    pa_source *source;

    pa_assert(m);
    pa_assert_se(u = m->userdata);
    pa_assert(u->card);

    PA_IDXSET_FOREACH(sink, u->card->sinks, idx)
        n += pa_sink_linked_by(sink);

    PA_IDXSET_FOREACH(source, u->card->sources, idx)
        n += pa_source_linked_by(source);

    return n;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        goto finish;

    if (u->mixers)
        pa_hashmap_free(u->mixers);
    if (u->jacks)
        pa_hashmap_free(u->jacks);

    if (u->card && u->card->sinks)
        pa_idxset_remove_all(u->card->sinks, (pa_free_cb_t) pa_alsa_sink_free);

    if (u->card && u->card->sources)
        pa_idxset_remove_all(u->card->sources, (pa_free_cb_t) pa_alsa_source_free);

    if (u->card)
        pa_card_free(u->card);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    if (u->profile_set)
        pa_alsa_profile_set_free(u->profile_set);

    pa_alsa_ucm_free(&u->ucm);

    pa_xfree(u->device_id);
    pa_xfree(u);

finish:
    pa_alsa_refcnt_dec();
}
