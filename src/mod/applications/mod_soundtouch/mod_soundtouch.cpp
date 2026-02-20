/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2024, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * mod_soundtouch.cpp -- SoundTouch Pitch-Preserving Time-Stretching for FreeSWITCH
 *
 * Integrates the SoundTouch audio processing library to provide tempo changes
 * without pitch shift (no "chipmunk effect") during file playback.
 *
 * Usage:
 *   Set channel variable use_soundtouch=true before playback to enable.
 *   Use speed:+1 or speed:-1 as usual in playback commands.
 *
 * API commands:
 *   soundtouch enable <uuid>              - Enable SoundTouch for session
 *   soundtouch disable <uuid>             - Disable SoundTouch for session
 *   soundtouch tempo <uuid> <value>       - Set tempo (0.5 to 2.0)
 */

#include <switch.h>
#include <SoundTouch.h>

using namespace soundtouch;

SWITCH_BEGIN_EXTERN_C
SWITCH_MODULE_LOAD_FUNCTION(mod_soundtouch_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_soundtouch_shutdown);
SWITCH_MODULE_DEFINITION(mod_soundtouch, mod_soundtouch_load, mod_soundtouch_shutdown, NULL);
SWITCH_END_EXTERN_C

#define ST_PRIVATE_KEY "soundtouch_session_data"
#define ST_BUFFER_SAMPLES 8192
#define ST_MIN_SPEED (-2)
#define ST_MAX_SPEED (2)

/* Per-session SoundTouch state */
struct st_session_data {
	SoundTouch *st;
	int current_speed;
	uint32_t rate;
	uint32_t channels;
};

static void st_session_data_destroy(st_session_data *sd)
{
	if (sd) {
		if (sd->st) {
			delete sd->st;
			sd->st = NULL;
		}
		free(sd);
	}
}

/* Session state handler to clean up SoundTouch state on session end */
static switch_status_t st_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	st_session_data *sd = (st_session_data *)switch_channel_get_private(channel, ST_PRIVATE_KEY);

	if (sd) {
		switch_channel_set_private(channel, ST_PRIVATE_KEY, NULL);
		st_session_data_destroy(sd);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_state_handler_table_t st_state_handlers = {
	/*.on_init */ NULL,
	/*.on_routing */ NULL,
	/*.on_execute */ NULL,
	/*.on_hangup */ st_on_hangup,
	/*.on_exchange_media */ NULL,
	/*.on_soft_execute */ NULL,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ NULL,
	/*.on_reset */ NULL,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ NULL
};

/*
 * Core integration function - called from switch_ivr_play_say.c when use_soundtouch=true.
 *
 * Takes input PCM samples, processes them through SoundTouch for tempo change
 * without pitch shift, and puts results into sp_audio_buffer for the playback
 * loop to drain.
 */
static switch_status_t soundtouch_speed_process(switch_core_session_t *session,
												int16_t *data,
												switch_size_t inlen,
												switch_buffer_t *sp_audio_buffer,
												int speed,
												uint32_t rate,
												uint32_t channels)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	st_session_data *sd = (st_session_data *)switch_channel_get_private(channel, ST_PRIVATE_KEY);
	uint32_t avail;
	SAMPLETYPE fbuf[ST_BUFFER_SAMPLES];
	int16_t obuf[ST_BUFFER_SAMPLES];
	float tempo;

	if (channels == 0) {
		channels = 1;
	}

	if (!sd) {
		sd = (st_session_data *)malloc(sizeof(st_session_data));
		if (!sd) {
			return SWITCH_STATUS_MEMERR;
		}
		sd->st = new SoundTouch();
		sd->current_speed = 0;
		sd->rate = 0;
		sd->channels = 0;
		switch_channel_set_private(channel, ST_PRIVATE_KEY, sd);
		switch_channel_add_state_handler(channel, &st_state_handlers);
	}

	/* Clamp speed to valid range */
	if (speed > ST_MAX_SPEED) speed = ST_MAX_SPEED;
	if (speed < ST_MIN_SPEED) speed = ST_MIN_SPEED;

	/* Reconfigure SoundTouch if settings changed */
	if (sd->rate != rate || sd->channels != channels || sd->current_speed != speed) {
		sd->st->setSampleRate(rate);
		sd->st->setChannels(channels);
		/* Convert FreeSWITCH speed (-2..+2) to SoundTouch tempo multiplier */
		tempo = 1.0f + (0.25f * (float)speed);
		sd->st->setTempo(tempo);
		sd->rate = rate;
		sd->channels = channels;
		sd->current_speed = speed;
	}

	/* Convert int16 input to float and feed into SoundTouch */
	{
		switch_size_t i;
		switch_size_t chunk = ST_BUFFER_SAMPLES;
		switch_size_t done = 0;

		while (done < inlen) {
			switch_size_t batch = inlen - done;
			if (batch > chunk) batch = chunk;
			for (i = 0; i < batch; i++) {
				fbuf[i] = (SAMPLETYPE)(data[done + i]) / 32768.0f;
			}
			sd->st->putSamples(fbuf, (uint32_t)(batch / channels));
			done += batch;
		}
	}

	/* Drain all available output samples from SoundTouch into sp_audio_buffer */
	for (;;) {
		uint32_t got;
		uint32_t max_per_channel = ST_BUFFER_SAMPLES / channels;

		avail = sd->st->numSamples();
		if (avail == 0) break;

		if (avail > max_per_channel) avail = max_per_channel;

		got = sd->st->receiveSamples(fbuf, avail);
		if (got == 0) break;

		{
			uint32_t i;
			uint32_t total = got * channels;
			for (i = 0; i < total; i++) {
				float s = fbuf[i] * 32768.0f;
				if (s > 32767.0f) s = 32767.0f;
				if (s < -32768.0f) s = -32768.0f;
				obuf[i] = (int16_t)s;
			}
			switch_buffer_write(sp_audio_buffer, obuf, total * sizeof(int16_t));
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

/* API: soundtouch enable <uuid> */
/* API: soundtouch disable <uuid> */
/* API: soundtouch tempo <uuid> <value> */
SWITCH_STANDARD_API(soundtouch_api_function)
{
	char *argv[4] = { 0 };
	int argc;
	char *mycmd = NULL;
	switch_core_session_t *rsession = NULL;
	switch_channel_t *rchannel = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (zstr(cmd)) {
		stream->write_function(stream, "-USAGE: soundtouch <enable|disable|tempo> <uuid> [value]\n");
		return SWITCH_STATUS_SUCCESS;
	}

	mycmd = strdup(cmd);
	argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

	if (argc < 2) {
		stream->write_function(stream, "-USAGE: soundtouch <enable|disable|tempo> <uuid> [value]\n");
		goto done;
	}

	if (!(rsession = switch_core_session_locate(argv[1]))) {
		stream->write_function(stream, "-ERR Session not found\n");
		goto done;
	}

	rchannel = switch_core_session_get_channel(rsession);

	if (!strcasecmp(argv[0], "enable")) {
		switch_channel_set_variable(rchannel, "use_soundtouch", "true");
		stream->write_function(stream, "+OK SoundTouch enabled for session %s\n", argv[1]);
	} else if (!strcasecmp(argv[0], "disable")) {
		switch_channel_set_variable(rchannel, "use_soundtouch", "false");
		/* Clean up existing SoundTouch state */
		{
			st_session_data *sd = (st_session_data *)switch_channel_get_private(rchannel, ST_PRIVATE_KEY);
			if (sd) {
				switch_channel_set_private(rchannel, ST_PRIVATE_KEY, NULL);
				st_session_data_destroy(sd);
			}
		}
		stream->write_function(stream, "+OK SoundTouch disabled for session %s\n", argv[1]);
	} else if (!strcasecmp(argv[0], "tempo")) {
		if (argc < 3) {
			stream->write_function(stream, "-USAGE: soundtouch tempo <uuid> <value>\n");
		} else {
			float tempo_val = (float)atof(argv[2]);
			char tempo_str[32];
			if (tempo_val < 0.5f) tempo_val = 0.5f;
			if (tempo_val > 2.0f) tempo_val = 2.0f;
			snprintf(tempo_str, sizeof(tempo_str), "%.3f", tempo_val);
			switch_channel_set_variable(rchannel, "soundtouch_tempo", tempo_str);
			switch_channel_set_variable(rchannel, "use_soundtouch", "true");
			stream->write_function(stream, "+OK SoundTouch tempo set to %s for session %s\n", tempo_str, argv[1]);
		}
	} else {
		stream->write_function(stream, "-ERR Unknown command: %s\n", argv[0]);
		status = SWITCH_STATUS_FALSE;
	}

	switch_core_session_rwunlock(rsession);

done:
	switch_safe_free(mycmd);
	return status;
}

/* Module globals for config settings */
static struct {
	switch_bool_t default_enabled;
	float min_tempo;
	float max_tempo;
} globals;

static switch_status_t do_config(void)
{
	switch_xml_t cfg, xml, settings, param;

	globals.default_enabled = SWITCH_TRUE;
	globals.min_tempo = 0.5f;
	globals.max_tempo = 2.0f;

	if (!(xml = switch_xml_open_cfg("soundtouch.conf", &cfg, NULL))) {
		return SWITCH_STATUS_SUCCESS;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *name = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");

			if (!strcasecmp(name, "default-enabled")) {
				globals.default_enabled = switch_true(val);
			} else if (!strcasecmp(name, "min-tempo")) {
				globals.min_tempo = (float)atof(val);
			} else if (!strcasecmp(name, "max-tempo")) {
				globals.max_tempo = (float)atof(val);
			}
		}
	}

	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_soundtouch_load)
{
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	do_config();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "SoundTouch module loaded (pitch-preserving time-stretching, "
					  "default-enabled=%s min-tempo=%.2f max-tempo=%.2f)\n",
					  globals.default_enabled ? "true" : "false",
					  globals.min_tempo, globals.max_tempo);

	/* Register the processing function so the core can use it */
	switch_ivr_soundtouch_process = soundtouch_speed_process;

	SWITCH_ADD_API(api_interface, "soundtouch",
				   "SoundTouch tempo control: soundtouch <enable|disable|tempo> <uuid> [value]",
				   soundtouch_api_function,
				   "<enable|disable|tempo> <uuid> [value]");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_soundtouch_shutdown)
{
	/* Unregister the processing function */
	switch_ivr_soundtouch_process = NULL;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "SoundTouch module unloaded\n");

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
