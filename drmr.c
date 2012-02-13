/* drmr.c
 * LV2 DrMr plugin
 * Copyright 2012 Nick Lanham <nick@afternight.org>
 *
 * Public License v3. source code is available at 
 * <http://github.com/nicklan/drmr>

 * THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "drmr.h"
#include "drmr_hydrogen.h"

static void* load_thread(void* arg) {
  DrMr* drmr = (DrMr*)arg;

  pthread_mutex_lock(&drmr->load_mutex);
  for(;;) {
    pthread_cond_wait(&drmr->load_cond,
		      &drmr->load_mutex);
    int request = (int)floorf(*(drmr->kitReq));
    if (request >= drmr->kits->num_kits) {
      int os = drmr->num_samples;
      drmr->num_samples = 0;
      if (os > 0) free_samples(drmr->samples,os);
      drmr->samples = NULL;
    } else
      load_hydrogen_kit(drmr,drmr->kits->kits[request].path);
    drmr->curKit = request;
  }
  pthread_mutex_unlock(&drmr->load_mutex);
  return 0;
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features) {
  int i;
  DrMr* drmr = malloc(sizeof(DrMr));
  drmr->map = NULL;
  drmr->num_samples = 0;

  if (pthread_mutex_init(&drmr->load_mutex, 0)) {
    fprintf(stderr, "Could not initialize load_mutex.\n");
    free(drmr);
    return 0;
  }
  if (pthread_cond_init(&drmr->load_cond, 0)) {
    fprintf(stderr, "Could not initialize load_cond.\n");
    free(drmr);
    return 0;
  }
  if (pthread_create(&drmr->load_thread, 0, load_thread, drmr)) {
    fprintf(stderr, "Could not initialize loading thread.\n");
    free(drmr);
    return 0;
  }

  // Map midi uri
  while(*features) {
    if (!strcmp((*features)->URI, LV2_URI_MAP_URI)) {
      drmr->map = (LV2_URI_Map_Feature *)((*features)->data);
      drmr->uris.midi_event = drmr->map->uri_to_id
	(drmr->map->callback_data,
	 "http://lv2plug.in/ns/ext/event",
	 "http://lv2plug.in/ns/ext/midi#MidiEvent");
    }
    features++;
  }
  if (!drmr->map) {
    fprintf(stderr, "LV2 host does not support uri-map.\n");
    free(drmr);
    return 0;
  }
  
  drmr->kits = scan_kits();
  if (!drmr->kits) {
    fprintf(stderr, "No drum kits found\n");
    free(drmr);
    return 0;
  }
  drmr->samples = NULL; // prevent attempted freeing in load
  load_hydrogen_kit(drmr,drmr->kits->kits->path);
  drmr->curKit = 0;

  drmr->gains = malloc(32*sizeof(float*));
  drmr->pans = malloc(32*sizeof(float*));
  for(i = 0;i<32;i++) {
    drmr->gains[i] = NULL;
    drmr->pans[i] = NULL;
  }

  return (LV2_Handle)drmr;
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data) {
  DrMr* drmr = (DrMr*)instance;
  DrMrPortIndex port_index = (DrMrPortIndex)port;
  switch (port_index) {
  case DRMR_MIDI:
    drmr->midi_port = (LV2_Event_Buffer*)data;
    break;
  case DRMR_LEFT:
    drmr->left = (float*)data;
    break;
  case DRMR_RIGHT:
    drmr->right = (float*)data;
    break;
  case DRMR_KITNUM:
    if(data) drmr->kitReq = (float*)data;
    break;
  default:
    break;
  }

  if (port_index >= DRMR_GAIN_ONE && port_index <= DRMR_GAIN_THIRTYTWO) {
    int goff = port_index - DRMR_GAIN_ONE;
    drmr->gains[goff] = (float*)data;
  }

  if (port_index >= DRMR_PAN_ONE && port_index <= DRMR_PAN_THIRTYTWO) {
    int poff = port_index - DRMR_PAN_ONE;
    drmr->pans[poff] = (float*)data;
  }
}

static inline void layer_to_sample(drmr_sample *sample, float gain) {
  int i;
  float mapped_gain = (1-(gain/GAIN_MIN));
  if (mapped_gain > 1.0f) mapped_gain = 1.0f;
  for(i = 0;i < sample->layer_count;i++) {
    if (sample->layers[i].min <= mapped_gain &&
	(sample->layers[i].max > mapped_gain ||
	 sample->layers[i].max == 1 && mapped_gain == 1)) {
      sample->limit = sample->layers[i].limit;
      sample->info = sample->layers[i].info;
      sample->data = sample->layers[i].data;
      return;
    }
  }
  fprintf(stderr,"Couldn't find layer for gain %f in sample\n\n",gain);
  sample->limit = 0;
  sample->info = NULL;
  sample->data = NULL;
}

#define DB3SCALE -0.8317830986718104f
#define DB3SCALEPO 1.8317830986718104f
// taken from lv2 example amp plugin
#define DB_CO(g) ((g) > GAIN_MIN ? powf(10.0f, (g) * 0.05f) : 0.0f)

static void run(LV2_Handle instance, uint32_t n_samples) {
  int i,kitInt;
  DrMr* drmr = (DrMr*)instance;

  kitInt = (int)floorf(*(drmr->kitReq));
  if (kitInt != drmr->curKit) // requested a new kit
    pthread_cond_signal(&drmr->load_cond);

  LV2_Event_Iterator eit;
  if (drmr->midi_port && lv2_event_begin(&eit,drmr->midi_port)) { // if we have any events
    LV2_Event *cur_ev;
    uint8_t* data;
    while (lv2_event_is_valid(&eit)) {
      cur_ev = lv2_event_get(&eit,&data);
      if (cur_ev->type == drmr->uris.midi_event) {
	int channel = *data & 15;
	switch ((*data) >> 4) {
	case 8:  // ignore note-offs for now, should probably be a setting
	  //if (drmr->cur_samp) drmr->cur_samp->active = 0;
	  break;
	case 9: {
	  uint8_t nn = data[1];
	  nn-=60; // middle c is our root note (setting?)
	  // need to mutex this to avoid getting the samples array
	  // changed after the check that the midi-note is valid
	  pthread_mutex_lock(&drmr->load_mutex); 
	  if (nn >= 0 && nn < drmr->num_samples) {
	    if (drmr->samples[nn].layer_count > 0)
	      layer_to_sample(drmr->samples+nn,*(drmr->gains[nn]));
	    if (drmr->samples[nn].limit == 0) {
	      printf("Fail at: %i for %f\n",nn,*drmr->gains[nn]);
	    }
	    drmr->samples[nn].active = 1;
	    drmr->samples[nn].offset = 0;
	  }
	  pthread_mutex_unlock(&drmr->load_mutex);
	  break;
	}
	default:
	  printf("Unhandeled status: %i\n",(*data)>>4);
	}
      } else printf("unrecognized event\n");
      lv2_event_increment(&eit);
    } 
  }

  for(i = 0;i<n_samples;i++) {
    drmr->left[i] = 0.0f;
    drmr->right[i] = 0.0f;
  }

  pthread_mutex_lock(&drmr->load_mutex); 
  for (i = 0;i < drmr->num_samples;i++) {
    int pos,lim;
    drmr_sample* cs = drmr->samples+i;
    if (cs->active && (cs->limit > 0)) {
      float coef_right, coef_left;
      if (i < 32) {
	float gain = DB_CO(*(drmr->gains[i]));
	float pan_right = ((*drmr->pans[i])+1)/2.0f;
	float pan_left = 1-pan_right;
	coef_right = (pan_right * (DB3SCALE * pan_right + DB3SCALEPO))*gain;
	coef_left = (pan_left * (DB3SCALE * pan_left + DB3SCALEPO))*gain;
      }
      else {
	coef_right = coef_left = 1.0f;
      }

      if (cs->info->channels == 1) { // play mono sample
	lim = (n_samples < (cs->limit - cs->offset)?n_samples:(cs->limit-cs->offset));
	for(pos = 0;pos < lim;pos++) {
	  drmr->left[pos]  += cs->data[cs->offset]*coef_left;
	  drmr->right[pos] += cs->data[cs->offset]*coef_right;
	  cs->offset++;
	}
      } else { // play stereo sample
	lim = (cs->limit-cs->offset)/cs->info->channels;
	if (lim > n_samples) lim = n_samples;
	for (pos=0;pos<lim;pos++) {
	  drmr->left[pos]  += cs->data[cs->offset++]*coef_left;
	  drmr->right[pos] += cs->data[cs->offset++]*coef_right;
	}
      }
      if (cs->offset >= cs->limit) cs->active = 0;
    }
  }
  pthread_mutex_unlock(&drmr->load_mutex); 
}

static void activate  (LV2_Handle instance) {}
static void deactivate(LV2_Handle instance) {}

static void cleanup(LV2_Handle instance) {
  DrMr* drmr = (DrMr*)instance;
  pthread_cancel(drmr->load_thread);
  pthread_join(drmr->load_thread, 0);
  if (drmr->num_samples > 0)
    free_samples(drmr->samples,drmr->num_samples);
  free(drmr->gains);
  free(instance);
}

static const void* extension_data(const char* uri) {
  return NULL;
}

static const LV2_Descriptor descriptor = {
  DRMR_URI,
  instantiate,
  connect_port,
  activate,
  run,
  deactivate,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}
