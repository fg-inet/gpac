/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018
 *					All rights reserved
 *
 *  This file is part of GPAC / audio output filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/modules/audio_out.h>
#include <gpac/thread.h>

#ifndef GPAC_DISABLE_AVILIB

#include <gpac/internal/avilib.h>


typedef struct
{
	//options
	char *drv;
	u32 bnum, bdur, threaded, priority;
	GF_Fraction dur;

	GF_FilterPid *pid;
	u32 sr, afmt, nb_ch, ch_cfg, timescale;

	GF_AudioOutput *audio_out;
	GF_Thread *th;
	u32 audio_th_state;
	Bool needs_recfg, wait_recfg;

	u32 pck_offset;
	u64 first_cts;
	Bool aborted;

} GF_AudioOutCtx;


void aout_reconfig(GF_AudioOutCtx *ctx)
{
	u32 sr, bps, old_bps, nb_ch, ch_cfg;
	GF_Err e = GF_OK;
	sr = ctx->sr;
	nb_ch = ctx->nb_ch;
	bps = old_bps = gf_audio_fmt_bit_depth(ctx->afmt);
	ch_cfg = ctx->ch_cfg;

	e = ctx->audio_out->ConfigureOutput(ctx->audio_out, &sr, &nb_ch, &bps, ch_cfg);
	if (e) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_AUDIO, ("[AudioOutput] Failed to configure audio output: %s\n", gf_error_to_string(e) ));
		if (bps!=16) bps = 16;
		if (sr!=44100) sr = 44100;
		if (nb_ch!=2) nb_ch = 2;
	}
	if ((sr != ctx->sr) || (nb_ch!=ctx->nb_ch) || (bps!=old_bps)) {
		gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(sr));
		switch (bps) {
		case 8:
			gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_U8));
			break;
		case 16:
			gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
			break;
		case 24:
			gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S24));
			break;
		case 32:
			gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S32));
			break;
		}
		gf_filter_pid_negociate_property(ctx->pid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(nb_ch));
		ctx->needs_recfg = GF_FALSE;
		//drop all packets until next reconfig
		ctx->wait_recfg = GF_TRUE;
	} else if (e==GF_OK) {
		ctx->needs_recfg = GF_FALSE;
		ctx->wait_recfg = GF_FALSE;
	}
}

u32 aout_th_proc(void *p)
{
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) p;

	ctx->audio_th_state = 1;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[AudioOutput] Entering audio thread ID %d\n", gf_th_id() ));

	while (ctx->audio_th_state == 1) {
		if (ctx->needs_recfg) {
			aout_reconfig(ctx);
		} else {
			ctx->audio_out->WriteAudio(ctx->audio_out);
		}
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[AudioOutput] Exiting audio thread\n"));
	ctx->audio_out->Shutdown(ctx->audio_out);
	ctx->audio_th_state = 3;
	return 0;
}


static u32 aout_fill_output(void *ptr, char *buffer, u32 buffer_size)
{
	u32 done = 0;
	GF_AudioOutCtx *ctx = ptr;

	memset(buffer, 0, buffer_size);
	if (!ctx->pid || ctx->aborted) return 0;

	while (done < buffer_size) {
		const char *data;
		u32 size;
		GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->pid);
		if (!pck) {
			return done;
		}
		if (ctx->needs_recfg) {
			return done;
		}
		data = gf_filter_pck_get_data(pck, &size);

		if (ctx->dur.den) {
			u64 cts = gf_filter_pck_get_cts(pck);
			if (!ctx->first_cts) ctx->first_cts = cts+1;

			if ((cts - ctx->first_cts + 1) * ctx->dur.den > ctx->dur.num*ctx->timescale) {
				gf_filter_pid_drop_packet(ctx->pid);
				if (!ctx->aborted) {
					GF_FilterEvent evt;
					GF_FEVT_INIT(evt, GF_FEVT_STOP, ctx->pid);
					gf_filter_pid_send_event(ctx->pid, &evt);

					ctx->aborted = GF_TRUE;
				}
				return done;
			}
		}

		if (data && !ctx->wait_recfg) {
			u32 nb_copy;
			assert(size >= ctx->pck_offset);
			
			nb_copy = (size - ctx->pck_offset);
			if (nb_copy + done > buffer_size) nb_copy = buffer_size - done;
			memcpy(buffer+done, data+ctx->pck_offset, nb_copy);
			done += nb_copy;
			if (nb_copy + ctx->pck_offset != size) {
				ctx->pck_offset = size - nb_copy - ctx->pck_offset;
				return done;
			}
			ctx->pck_offset = 0;

		}
		gf_filter_pid_drop_packet(ctx->pid);
	}

	return done;
}

void aout_set_priority(GF_AudioOutCtx *ctx, u32 prio)
{
	if (prio==ctx->priority) return;
	ctx->priority = prio;
	if (ctx->th) gf_th_set_priority(ctx->th, (s32) ctx->priority);
	else if (ctx->audio_out->SelfThreaded && ctx->audio_out->SetPriority)
		ctx->audio_out->SetPriority(ctx->audio_out, ctx->priority);
}

static GF_Err aout_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	u32 sr, nb_ch, afmt, ch_cfg, timescale;
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) gf_filter_get_udta(filter);

	if (is_remove) {
		assert(ctx->pid==pid);
		ctx->pid=NULL;
		return GF_OK;
	}
	assert(!ctx->pid || (ctx->pid==pid));
	gf_filter_pid_check_caps(pid);

	sr = afmt = nb_ch = ch_cfg = timescale = 0;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p) timescale = p->value.uint;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
	if (p) sr = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_AUDIO_FORMAT);
	if (p) afmt = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
	if (p) nb_ch = p->value.uint;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CHANNEL_LAYOUT);
	if (p) ch_cfg = p->value.uint;

	if ((ctx->sr==sr) && (ctx->afmt == afmt) && (ctx->nb_ch == nb_ch) && (ctx->ch_cfg == ch_cfg)) return GF_OK;

	if (!ctx->pid) {
		GF_FilterEvent evt;
		GF_FEVT_INIT(evt, GF_FEVT_PLAY, pid);
		gf_filter_pid_send_event(pid, &evt);
	}
	ctx->pid = pid;
	ctx->timescale = timescale;
	ctx->sr = sr;
	ctx->afmt = afmt;
	ctx->nb_ch = nb_ch;
	ctx->ch_cfg = ch_cfg;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_AUDIO_PRIORITY);
	if (p) aout_set_priority(ctx, p->value.uint);

	ctx->needs_recfg = GF_TRUE;
	//not threaded, request a task to restart audio (cannot do it during the audio callback)
	if (!ctx->th) gf_filter_post_process_task(filter);
	return GF_OK;
}

static GF_Err aout_initialize(GF_Filter *filter)
{
	const char *sOpt;
	GF_Err e;
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) gf_filter_get_udta(filter);
	GF_User *user = gf_fs_get_user( gf_filter_get_session(filter) );

	if (!user) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_AUDIO, ("[AudioOut] No user/modules defined, cannot load audio output\n"));
		return GF_IO_ERR;
	}
	if (ctx->drv) {
		ctx->audio_out = (GF_AudioOutput *) gf_modules_load_interface_by_name(user->modules, ctx->drv, GF_AUDIO_OUTPUT_INTERFACE);
	}
	/*get a prefered compositor*/
	if (!ctx->audio_out) {
		sOpt = gf_cfg_get_key(user->config, "Audio", "DriverName");
		if (sOpt) {
			ctx->audio_out = (GF_AudioOutput *) gf_modules_load_interface_by_name(user->modules, sOpt, GF_AUDIO_OUTPUT_INTERFACE);
		}
	}
	if (!ctx->audio_out) {
		u32 i, count = gf_modules_get_count(user->modules);
		for (i=0; i<count; i++) {
			ctx->audio_out = (GF_AudioOutput *) gf_modules_load_interface(user->modules, i, GF_AUDIO_OUTPUT_INTERFACE);
			if (!ctx->audio_out) continue;

			//no more support for raw out, deprecated
			if (!stricmp(ctx->audio_out->module_name, "Raw Audio Output")) {
				gf_modules_close_interface((GF_BaseInterface *)ctx->audio_out);
				ctx->audio_out = NULL;
				continue;
			}
			GF_LOG(GF_LOG_DEBUG, GF_LOG_AUDIO, ("[AudioOut] Audio output module %s loaded\n", ctx->audio_out->module_name));
			/*check that's a valid audio compositor*/
			if ((ctx->audio_out->SelfThreaded && ctx->audio_out->SetPriority) || ctx->audio_out->WriteAudio) {
				/*remember the module we use*/
				gf_cfg_set_key(user->config, "Audio", "DriverName", ctx->audio_out->module_name);
				break;
			}
			gf_modules_close_interface((GF_BaseInterface *)ctx->audio_out);
			ctx->audio_out = NULL;
		}
	}

	/*if not init we run with a NULL audio compositor*/
	if (!ctx->audio_out) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_AUDIO, ("[AudioOut] No audio output modules found, cannot load audio output\n"));
		return GF_IO_ERR;
	}

	ctx->audio_out->FillBuffer = aout_fill_output;
	ctx->audio_out->audio_renderer = ctx;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_AUDIO, ("[AudioOut] Setting up audio module %s\n", ctx->audio_out->module_name));


	sOpt = gf_cfg_get_key(user->config, "Audio", "ForceConfig");
	if (sOpt && !stricmp(sOpt, "yes") && (!ctx->bnum || !ctx->bdur) ) {
		sOpt = gf_cfg_get_key(user->config, "Audio", "NumBuffers");
		ctx->bnum = sOpt ? atoi(sOpt) : 6;
		sOpt = gf_cfg_get_key(user->config, "Audio", "TotalDuration");
		ctx->bdur = sOpt ? atoi(sOpt) : 400;
	}

	e = ctx->audio_out->Setup(ctx->audio_out, user->os_window_handler, ctx->bnum, ctx->bdur);

	if (e != GF_OK) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_AUDIO, ("[AudioOut] Could not setup module %s\n", ctx->audio_out->module_name));
		gf_modules_close_interface((GF_BaseInterface *)ctx->audio_out);
		ctx->audio_out = NULL;
		return e;
	}

	if (ctx->audio_out->SelfThreaded) {
		if (ctx->audio_out->SetPriority)
			ctx->audio_out->SetPriority(ctx->audio_out, GF_THREAD_PRIORITY_REALTIME);
	} else if (ctx->threaded) {
		ctx->th = gf_th_new("AudioOutput");
		gf_th_run(ctx->th, aout_th_proc, ctx);
	}

	return GF_OK;
}

static void aout_finalize(GF_Filter *filter)
{
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) gf_filter_get_udta(filter);

	/*stop and shutdown*/
	if (ctx->audio_out) {
		/*kill audio thread*/
		if (ctx->th) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_AUDIO, ("[AudioOut] stopping audio thread\n"));
			ctx->audio_th_state = 2;
			while (ctx->audio_th_state != 3) {
				gf_sleep(33);
			}
			GF_LOG(GF_LOG_DEBUG, GF_LOG_AUDIO, ("[AudioOut] audio thread stopped\n"));
			gf_th_del(ctx->th);
		} else {
			ctx->audio_out->Shutdown(ctx->audio_out);
		}
		gf_modules_close_interface((GF_BaseInterface *)ctx->audio_out);
		ctx->audio_out = NULL;
	}
}

static GF_Err aout_process(GF_Filter *filter)
{
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) gf_filter_get_udta(filter);

	if (!ctx->th && ctx->needs_recfg) {
		aout_reconfig(ctx);
	}

	if (ctx->th || ctx->audio_out->SelfThreaded) return GF_EOS;

	ctx->audio_out->WriteAudio(ctx->audio_out);
	return GF_OK;
}

static Bool aout_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	const GF_PropertyValue *p;
	GF_AudioOutCtx *ctx = (GF_AudioOutCtx *) gf_filter_get_udta(filter);
	if (!ctx->audio_out) return GF_TRUE;

	switch (evt->base.type) {
	case GF_FEVT_PLAY:
		if (ctx->audio_out->Play) ctx->audio_out->Play(ctx->audio_out, evt->play.hw_buffer_reset ? 2 : 1);
		break;
	case GF_FEVT_STOP:
		if (ctx->audio_out->Play) ctx->audio_out->Play(ctx->audio_out, 0);
		break;
	case GF_FEVT_INFO_UPDATE:
		if (ctx->audio_out->SetVolume) {
			p = gf_filter_pid_get_info(ctx->pid, GF_PROP_PID_AUDIO_VOLUME);
			if (p) ctx->audio_out->SetVolume(ctx->audio_out, p->value.uint);
		}
		if (ctx->audio_out->SetPan) {
			p = gf_filter_pid_get_info(ctx->pid, GF_PROP_PID_AUDIO_PAN);
			if (p) ctx->audio_out->SetPan(ctx->audio_out, p->value.uint);
		}
		p = gf_filter_pid_get_property(ctx->pid, GF_PROP_PID_AUDIO_PRIORITY);
		if (p) aout_set_priority(ctx, p->value.uint);

		break;
	default:
		break;
	}
	//cancel
	return GF_TRUE;
}

#define OFFS(_n)	#_n, offsetof(GF_AudioOutCtx, _n)

static const GF_FilterArgs AudioOutArgs[] =
{
	{ OFFS(drv), "audio driver name", GF_PROP_NAME, NULL, NULL, GF_FALSE},
	{ OFFS(bnum), "number of audio buffers - 0 for auto", GF_PROP_UINT, "0", NULL, GF_FALSE},
	{ OFFS(bdur), "total duration of all buffers in ms - 0 for auto", GF_PROP_UINT, "0", NULL, GF_FALSE},
	{ OFFS(threaded), "force dedicated thread creation if sound card driver is not threaded", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(dur), "only plays the specified duration", GF_PROP_FRACTION, "0", NULL, GF_FALSE},

	{}
};

static const GF_FilterCapability AudioOutInputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_INC_UINT(GF_PROP_PID_CODECID, GF_CODECID_RAW),
	CAP_INC_UINT(GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S16),
	CAP_INC_UINT(GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S24),
};


GF_FilterRegister AudioOutRegister = {
	.name = "aout",
	.description = "Audio Output",
	.private_size = sizeof(GF_AudioOutCtx),
	.args = AudioOutArgs,
	INCAPS(AudioOutInputs),
	.initialize = aout_initialize,
	.finalize = aout_finalize,
	.configure_pid = aout_configure_pid,
	.process = aout_process,
	.process_event = aout_process_event
};

#endif

const GF_FilterRegister *aout_register(GF_FilterSession *session)
{
#ifndef GPAC_DISABLE_AVILIB
	return &AudioOutRegister;
#else
	return NULL;
#endif

}
