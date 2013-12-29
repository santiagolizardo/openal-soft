
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "midi/base.h"

#include "alMain.h"
#include "alError.h"
#include "alMidi.h"
#include "evtqueue.h"
#include "rwlock.h"
#include "alu.h"

#ifdef HAVE_FLUIDSYNTH

#include <fluidsynth.h>


/* MIDI events */
#define SYSEX_EVENT  (0xF0)

/* MIDI controllers */
#define CTRL_BANKSELECT_MSB  (0)
#define CTRL_BANKSELECT_LSB  (32)
#define CTRL_ALLNOTESOFF     (123)


typedef struct FSample {
    DERIVE_FROM_TYPE(fluid_sample_t);

    ALfontsound *Sound;

    fluid_mod_t *Mods;
    ALsizei NumMods;
} FSample;

static void FSample_Construct(FSample *self, ALfontsound *sound, ALsoundfont *sfont)
{
    fluid_sample_t *sample = STATIC_CAST(fluid_sample_t, self);
    memset(sample->name, 0, sizeof(sample->name));
    sample->start = sound->Start;
    sample->end = sound->End;
    sample->loopstart = sound->LoopStart;
    sample->loopend = sound->LoopEnd;
    sample->samplerate = sound->SampleRate;
    sample->origpitch = sound->PitchKey;
    sample->pitchadj = sound->PitchCorrection;
    sample->sampletype = sound->SampleType;
    sample->valid = 1;
    sample->data = sfont->Samples;

    sample->amplitude_that_reaches_noise_floor_is_valid = 0;
    sample->amplitude_that_reaches_noise_floor = 0.0;

    sample->refcount = 0;

    sample->notify = NULL;

    sample->userdata = self;

    self->Sound = sound;

    self->Mods = NULL;
    self->NumMods = 0;
}

static void FSample_Destruct(FSample *self)
{
    free(self->Mods);
    self->Mods = NULL;
    self->NumMods = 0;
}


typedef struct FPreset {
    DERIVE_FROM_TYPE(fluid_preset_t);

    char Name[16];

    int Preset;
    int Bank;

    FSample *Samples;
    ALsizei NumSamples;
} FPreset;

static char* FPreset_getName(fluid_preset_t *preset);
static int FPreset_getPreset(fluid_preset_t *preset);
static int FPreset_getBank(fluid_preset_t *preset);
static int FPreset_noteOn(fluid_preset_t *preset, fluid_synth_t *synth, int channel, int key, int velocity);

static void FPreset_Construct(FPreset *self, ALsfpreset *preset, fluid_sfont_t *parent, ALsoundfont *sfont)
{
    STATIC_CAST(fluid_preset_t, self)->data = self;
    STATIC_CAST(fluid_preset_t, self)->sfont = parent;
    STATIC_CAST(fluid_preset_t, self)->free = NULL;
    STATIC_CAST(fluid_preset_t, self)->get_name = FPreset_getName;
    STATIC_CAST(fluid_preset_t, self)->get_banknum = FPreset_getBank;
    STATIC_CAST(fluid_preset_t, self)->get_num = FPreset_getPreset;
    STATIC_CAST(fluid_preset_t, self)->noteon = FPreset_noteOn;
    STATIC_CAST(fluid_preset_t, self)->notify = NULL;

    memset(self->Name, 0, sizeof(self->Name));
    self->Preset = preset->Preset;
    self->Bank = preset->Bank;

    self->NumSamples = 0;
    self->Samples = calloc(1, preset->NumSounds * sizeof(self->Samples[0]));
    if(self->Samples)
    {
        ALsizei i;
        self->NumSamples = preset->NumSounds;
        for(i = 0;i < self->NumSamples;i++)
            FSample_Construct(&self->Samples[i], preset->Sounds[i], sfont);
    }
}

static void FPreset_Destruct(FPreset *self)
{
    ALsizei i;

    for(i = 0;i < self->NumSamples;i++)
        FSample_Destruct(&self->Samples[i]);
    free(self->Samples);
    self->Samples = NULL;
    self->NumSamples = 0;
}

static char* FPreset_getName(fluid_preset_t *preset)
{
    return ((FPreset*)preset->data)->Name;
}

static int FPreset_getPreset(fluid_preset_t *preset)
{
    return ((FPreset*)preset->data)->Preset;
}

static int FPreset_getBank(fluid_preset_t *preset)
{
    return ((FPreset*)preset->data)->Bank;
}

static int FPreset_noteOn(fluid_preset_t *preset, fluid_synth_t *synth, int channel, int key, int vel)
{
    FPreset *self = ((FPreset*)preset->data);
    ALsizei i;

    for(i = 0;i < self->NumSamples;i++)
    {
        FSample *sample = &self->Samples[i];
        ALfontsound *sound = sample->Sound;
        fluid_voice_t *voice;
        ALsizei m;

        if(!(key >= sound->MinKey && key <= sound->MaxKey && vel >= sound->MinVelocity && vel <= sound->MaxVelocity))
            continue;

        voice = fluid_synth_alloc_voice(synth, STATIC_CAST(fluid_sample_t, sample), channel, key, vel);
        if(voice == NULL)
            return FLUID_FAILED;

        fluid_voice_gen_set(voice,  5, sound->ModLfoToPitch);
        fluid_voice_gen_set(voice,  6, sound->VibratoLfoToPitch);
        fluid_voice_gen_set(voice,  7, sound->ModEnvToPitch);
        fluid_voice_gen_set(voice,  8, sound->FilterCutoff);
        fluid_voice_gen_set(voice,  9, sound->FilterQ);
        fluid_voice_gen_set(voice, 10, sound->ModLfoToFilterCutoff);
        fluid_voice_gen_set(voice, 11, sound->ModEnvToFilterCutoff);
        fluid_voice_gen_set(voice, 25, sound->ModEnv.DelayTime);
        fluid_voice_gen_set(voice, 26, sound->ModEnv.AttackTime);
        fluid_voice_gen_set(voice, 27, sound->ModEnv.HoldTime);
        fluid_voice_gen_set(voice, 28, sound->ModEnv.DecayTime);
        fluid_voice_gen_set(voice, 29, sound->ModEnv.SustainVol);
        fluid_voice_gen_set(voice, 30, sound->ModEnv.ReleaseTime);
        fluid_voice_gen_set(voice, 31, sound->ModEnv.KeyToHoldTime);
        fluid_voice_gen_set(voice, 32, sound->ModEnv.KeyToDecayTime);
        fluid_voice_gen_set(voice, 33, sound->VolEnv.DelayTime);
        fluid_voice_gen_set(voice, 34, sound->VolEnv.AttackTime);
        fluid_voice_gen_set(voice, 35, sound->VolEnv.HoldTime);
        fluid_voice_gen_set(voice, 36, sound->VolEnv.DecayTime);
        fluid_voice_gen_set(voice, 37, sound->VolEnv.SustainVol);
        fluid_voice_gen_set(voice, 38, sound->VolEnv.ReleaseTime);
        fluid_voice_gen_set(voice, 39, sound->VolEnv.KeyToHoldTime);
        fluid_voice_gen_set(voice, 40, sound->VolEnv.KeyToDecayTime);
        fluid_voice_gen_set(voice, 51, sound->CoarseTuning);
        fluid_voice_gen_set(voice, 52, sound->FineTuning);
        fluid_voice_gen_set(voice, 54, sound->LoopMode);
        fluid_voice_gen_set(voice, 56, sound->TuningScale);
        for(m = 0;m < sample->NumMods;m++)
            fluid_voice_add_mod(voice, &sample->Mods[m], FLUID_VOICE_OVERWRITE);

        fluid_synth_start_voice(synth, voice);
    }

    return FLUID_OK;
}


typedef struct FSfont {
    DERIVE_FROM_TYPE(fluid_sfont_t);

    char Name[16];

    FPreset *Presets;
    ALsizei NumPresets;

    ALsizei CurrentPos;
} FSfont;

static int FSfont_free(fluid_sfont_t *sfont);
static char* FSfont_getName(fluid_sfont_t *sfont);
static fluid_preset_t* FSfont_getPreset(fluid_sfont_t *sfont, unsigned int bank, unsigned int prenum);
static void FSfont_iterStart(fluid_sfont_t *sfont);
static int FSfont_iterNext(fluid_sfont_t *sfont, fluid_preset_t *preset);

static void FSfont_Construct(FSfont *self, ALsoundfont *sfont)
{
    STATIC_CAST(fluid_sfont_t, self)->data = self;
    STATIC_CAST(fluid_sfont_t, self)->id = FLUID_FAILED;
    STATIC_CAST(fluid_sfont_t, self)->free = FSfont_free;
    STATIC_CAST(fluid_sfont_t, self)->get_name = FSfont_getName;
    STATIC_CAST(fluid_sfont_t, self)->get_preset = FSfont_getPreset;
    STATIC_CAST(fluid_sfont_t, self)->iteration_start = FSfont_iterStart;
    STATIC_CAST(fluid_sfont_t, self)->iteration_next = FSfont_iterNext;

    memset(self->Name, 0, sizeof(self->Name));
    self->CurrentPos = 0;
    self->NumPresets = 0;
    self->Presets = calloc(1, sfont->NumPresets * sizeof(self->Presets[0]));
    if(self->Presets)
    {
        ALsizei i;
        self->NumPresets = sfont->NumPresets;
        for(i = 0;i < self->NumPresets;i++)
            FPreset_Construct(&self->Presets[i], sfont->Presets[i], STATIC_CAST(fluid_sfont_t, self), sfont);
    }
}

static void FSfont_Destruct(FSfont *self)
{
    ALsizei i;

    for(i = 0;i < self->NumPresets;i++)
        FPreset_Destruct(&self->Presets[i]);
    free(self->Presets);
    self->Presets = NULL;
    self->NumPresets = 0;
    self->CurrentPos = 0;
}

static int FSfont_free(fluid_sfont_t *sfont)
{
    FSfont *self = STATIC_UPCAST(FSfont, fluid_sfont_t, sfont);
    FSfont_Destruct(self);
    free(self);
    return 0;
}

static char* FSfont_getName(fluid_sfont_t *sfont)
{
    return STATIC_UPCAST(FSfont, fluid_sfont_t, sfont)->Name;
}

static fluid_preset_t *FSfont_getPreset(fluid_sfont_t *sfont, unsigned int bank, unsigned int prenum)
{
    FSfont *self = STATIC_UPCAST(FSfont, fluid_sfont_t, sfont);
    ALsizei i;

    for(i = 0;i < self->NumPresets;i++)
    {
        FPreset *preset = &self->Presets[i];
        if(preset->Bank == (int)bank && preset->Preset == (int)prenum)
            return STATIC_CAST(fluid_preset_t, preset);
    }

    return NULL;
}

static void FSfont_iterStart(fluid_sfont_t *sfont)
{
    STATIC_UPCAST(FSfont, fluid_sfont_t, sfont)->CurrentPos = 0;
}

static int FSfont_iterNext(fluid_sfont_t *sfont, fluid_preset_t *preset)
{
    FSfont *self = STATIC_UPCAST(FSfont, fluid_sfont_t, sfont);
    if(self->CurrentPos >= self->NumPresets)
        return 0;
    *preset = *STATIC_CAST(fluid_preset_t, &self->Presets[self->CurrentPos++]);
    preset->free = NULL;
    return 1;
}


typedef struct FSynth {
    DERIVE_FROM_TYPE(MidiSynth);
    DERIVE_FROM_TYPE(fluid_sfloader_t);

    fluid_settings_t *Settings;
    fluid_synth_t *Synth;
    int *FontIDs;
    ALsizei NumFontIDs;

    ALboolean ForceGM2BankSelect;
} FSynth;

static void FSynth_Construct(FSynth *self, ALCdevice *device);
static void FSynth_Destruct(FSynth *self);
static ALboolean FSynth_init(FSynth *self, ALCdevice *device);
static ALboolean FSynth_isSoundfont(FSynth *self, const char *filename);
static ALenum FSynth_loadSoundfont(FSynth *self, const char *filename);
static ALenum FSynth_selectSoundfonts(FSynth *self, ALCdevice *device, ALsizei count, const ALuint *ids);
static void FSynth_setGain(FSynth *self, ALfloat gain);
static void FSynth_setState(FSynth *self, ALenum state);
static void FSynth_stop(FSynth *self);
static void FSynth_reset(FSynth *self);
static void FSynth_update(FSynth *self, ALCdevice *device);
static void FSynth_processQueue(FSynth *self, ALuint64 time);
static void FSynth_process(FSynth *self, ALuint SamplesToDo, ALfloat (*restrict DryBuffer)[BUFFERSIZE]);
static void FSynth_Delete(FSynth *self);
DEFINE_MIDISYNTH_VTABLE(FSynth);

static fluid_sfont_t *FSynth_loadSfont(fluid_sfloader_t *loader, const char *filename);


static void FSynth_Construct(FSynth *self, ALCdevice *device)
{
    MidiSynth_Construct(STATIC_CAST(MidiSynth, self), device);
    SET_VTABLE2(FSynth, MidiSynth, self);

    STATIC_CAST(fluid_sfloader_t, self)->data = self;
    STATIC_CAST(fluid_sfloader_t, self)->free = NULL;
    STATIC_CAST(fluid_sfloader_t, self)->load = FSynth_loadSfont;

    self->Settings = NULL;
    self->Synth = NULL;
    self->FontIDs = NULL;
    self->NumFontIDs = 0;
    self->ForceGM2BankSelect = AL_FALSE;
}

static void FSynth_Destruct(FSynth *self)
{
    ALsizei i;

    for(i = 0;i < self->NumFontIDs;i++)
        fluid_synth_sfunload(self->Synth, self->FontIDs[i], 0);
    free(self->FontIDs);
    self->FontIDs = NULL;
    self->NumFontIDs = 0;

    if(self->Synth != NULL)
        delete_fluid_synth(self->Synth);
    self->Synth = NULL;

    if(self->Settings != NULL)
        delete_fluid_settings(self->Settings);
    self->Settings = NULL;

    MidiSynth_Destruct(STATIC_CAST(MidiSynth, self));
}

static ALboolean FSynth_init(FSynth *self, ALCdevice *device)
{
    self->Settings = new_fluid_settings();
    if(!self->Settings)
    {
        ERR("Failed to create FluidSettings\n");
        return AL_FALSE;
    }

    fluid_settings_setint(self->Settings, "synth.polyphony", 256);
    fluid_settings_setnum(self->Settings, "synth.sample-rate", device->Frequency);

    self->Synth = new_fluid_synth(self->Settings);
    if(!self->Synth)
    {
        ERR("Failed to create FluidSynth\n");
        return AL_FALSE;
    }

    fluid_synth_add_sfloader(self->Synth, STATIC_CAST(fluid_sfloader_t, self));

    return AL_TRUE;
}


static fluid_sfont_t *FSynth_loadSfont(fluid_sfloader_t *loader, const char *filename)
{
    FSynth *self = STATIC_UPCAST(FSynth, fluid_sfloader_t, loader);
    FSfont *sfont;
    int idx;

    if(!filename || sscanf(filename, "_al_internal %d", &idx) != 1)
        return NULL;
    if(idx >= STATIC_CAST(MidiSynth, self)->NumSoundfonts)
    {
        ERR("Received invalid soundfont index %d (max: %d)\n", idx, STATIC_CAST(MidiSynth, self)->NumSoundfonts);
        return NULL;
    }

    sfont = calloc(1, sizeof(sfont[0]));
    if(!sfont) return NULL;

    FSfont_Construct(sfont, STATIC_CAST(MidiSynth, self)->Soundfonts[idx]);
    return STATIC_CAST(fluid_sfont_t, sfont);
}

static ALboolean FSynth_isSoundfont(FSynth *self, const char *filename)
{
    filename = MidiSynth_getFontName(STATIC_CAST(MidiSynth, self), filename);
    if(!filename[0]) return AL_FALSE;

    if(!fluid_is_soundfont(filename))
        return AL_FALSE;
    return AL_TRUE;
}

static ALenum FSynth_loadSoundfont(FSynth *self, const char *filename)
{
    int *fontid;
    ALsizei count;
    ALsizei i;

    filename = MidiSynth_getFontName(STATIC_CAST(MidiSynth, self), filename);
    if(!filename[0]) return AL_INVALID_VALUE;

    fontid = malloc(sizeof(fontid[0]));
    if(!fontid) return AL_OUT_OF_MEMORY;

    fontid[0] = fluid_synth_sfload(self->Synth, filename, 1);
    if(fontid[0] == FLUID_FAILED)
    {
        ERR("Failed to load soundfont '%s'\n", filename);
        free(fontid);
        return AL_INVALID_VALUE;
    }

    fontid = ExchangePtr((XchgPtr*)&self->FontIDs, fontid);
    count = ExchangeInt(&self->NumFontIDs, 1);

    for(i = 0;i < count;i++)
        fluid_synth_sfunload(self->Synth, fontid[i], 1);
    free(fontid);

    return AL_NO_ERROR;
}

static ALenum FSynth_selectSoundfonts(FSynth *self, ALCdevice *device, ALsizei count, const ALuint *ids)
{
    int *fontid;
    ALenum ret;
    ALsizei i;

    ret = MidiSynth_selectSoundfonts(STATIC_CAST(MidiSynth, self), device, count, ids);
    if(ret != AL_NO_ERROR) return ret;

    fontid = malloc(count * sizeof(fontid[0]));
    if(fontid)
    {
        for(i = 0;i < STATIC_CAST(MidiSynth, self)->NumSoundfonts;i++)
        {
            char name[16];
            snprintf(name, sizeof(name), "_al_internal %d", i);

            fontid[i] = fluid_synth_sfload(self->Synth, name, 1);
            if(fontid[i] == FLUID_FAILED)
                ERR("Failed to load selected soundfont %d\n", i);
        }

        fontid = ExchangePtr((XchgPtr*)&self->FontIDs, fontid);
        count = ExchangeInt(&self->NumFontIDs, count);
    }
    else
    {
        ERR("Failed to allocate space for %d font IDs!\n", count);
        fontid = ExchangePtr((XchgPtr*)&self->FontIDs, NULL);
        count = ExchangeInt(&self->NumFontIDs, 0);
    }

    for(i = 0;i < count;i++)
        fluid_synth_sfunload(self->Synth, fontid[i], 1);
    free(fontid);

    return ret;
}


static void FSynth_setGain(FSynth *self, ALfloat gain)
{
    /* Scale gain by an additional 0.2 (-14dB), to help keep the mix from clipping. */
    fluid_settings_setnum(self->Settings, "synth.gain", 0.2 * gain);
    fluid_synth_set_gain(self->Synth, 0.2f * gain);
    MidiSynth_setGain(STATIC_CAST(MidiSynth, self), gain);
}


static void FSynth_setState(FSynth *self, ALenum state)
{
    MidiSynth_setState(STATIC_CAST(MidiSynth, self), state);
}

static void FSynth_stop(FSynth *self)
{
    MidiSynth *synth = STATIC_CAST(MidiSynth, self);
    ALsizei chan;

    /* Make sure all pending events are processed. */
    while(!(synth->SamplesToNext >= 1.0))
    {
        ALuint64 time = synth->NextEvtTime;
        if(time == UINT64_MAX)
            break;

        synth->SamplesSinceLast -= (time - synth->LastEvtTime) * synth->SamplesPerTick;
        synth->SamplesSinceLast = maxd(synth->SamplesSinceLast, 0.0);
        synth->LastEvtTime = time;
        FSynth_processQueue(self, time);

        synth->NextEvtTime = MidiSynth_getNextEvtTime(synth);
        if(synth->NextEvtTime != UINT64_MAX)
            synth->SamplesToNext += (synth->NextEvtTime - synth->LastEvtTime) * synth->SamplesPerTick;
    }

    /* All notes off */
    for(chan = 0;chan < 16;chan++)
        fluid_synth_cc(self->Synth, chan, CTRL_ALLNOTESOFF, 0);

    MidiSynth_stop(STATIC_CAST(MidiSynth, self));
}

static void FSynth_reset(FSynth *self)
{
    /* Reset to power-up status. */
    fluid_synth_system_reset(self->Synth);

    MidiSynth_reset(STATIC_CAST(MidiSynth, self));
}


static void FSynth_update(FSynth *self, ALCdevice *device)
{
    fluid_settings_setnum(self->Settings, "synth.sample-rate", device->Frequency);
    fluid_synth_set_sample_rate(self->Synth, device->Frequency);
    MidiSynth_update(STATIC_CAST(MidiSynth, self), device);
}


static void FSynth_processQueue(FSynth *self, ALuint64 time)
{
    EvtQueue *queue = &STATIC_CAST(MidiSynth, self)->EventQueue;

    while(queue->pos < queue->size && queue->events[queue->pos].time <= time)
    {
        const MidiEvent *evt = &queue->events[queue->pos];

        if(evt->event == SYSEX_EVENT)
        {
            static const ALbyte gm2_on[] = { 0x7E, 0x7F, 0x09, 0x03 };
            static const ALbyte gm2_off[] = { 0x7E, 0x7F, 0x09, 0x02 };
            int handled = 0;

            fluid_synth_sysex(self->Synth, evt->param.sysex.data, evt->param.sysex.size, NULL, NULL, &handled, 0);
            if(!handled && evt->param.sysex.size >= (ALsizei)sizeof(gm2_on))
            {
                if(memcmp(evt->param.sysex.data, gm2_on, sizeof(gm2_on)) == 0)
                    self->ForceGM2BankSelect = AL_TRUE;
                else if(memcmp(evt->param.sysex.data, gm2_off, sizeof(gm2_off)) == 0)
                    self->ForceGM2BankSelect = AL_FALSE;
            }
        }
        else switch((evt->event&0xF0))
        {
            case AL_NOTEOFF_SOFT:
                fluid_synth_noteoff(self->Synth, (evt->event&0x0F), evt->param.val[0]);
                break;
            case AL_NOTEON_SOFT:
                fluid_synth_noteon(self->Synth, (evt->event&0x0F), evt->param.val[0], evt->param.val[1]);
                break;
            case AL_AFTERTOUCH_SOFT:
                break;

            case AL_CONTROLLERCHANGE_SOFT:
                if(self->ForceGM2BankSelect)
                {
                    int chan = (evt->event&0x0F);
                    if(evt->param.val[0] == CTRL_BANKSELECT_MSB)
                    {
                        if(evt->param.val[1] == 120 && (chan == 9 || chan == 10))
                            fluid_synth_set_channel_type(self->Synth, chan, CHANNEL_TYPE_DRUM);
                        else if(evt->param.val[1] == 121)
                            fluid_synth_set_channel_type(self->Synth, chan, CHANNEL_TYPE_MELODIC);
                        break;
                    }
                    if(evt->param.val[0] == CTRL_BANKSELECT_LSB)
                    {
                        fluid_synth_bank_select(self->Synth, chan, evt->param.val[1]);
                        break;
                    }
                }
                fluid_synth_cc(self->Synth, (evt->event&0x0F), evt->param.val[0], evt->param.val[1]);
                break;
            case AL_PROGRAMCHANGE_SOFT:
                fluid_synth_program_change(self->Synth, (evt->event&0x0F), evt->param.val[0]);
                break;

            case AL_CHANNELPRESSURE_SOFT:
                fluid_synth_channel_pressure(self->Synth, (evt->event&0x0F), evt->param.val[0]);
                break;

            case AL_PITCHBEND_SOFT:
                fluid_synth_pitch_bend(self->Synth, (evt->event&0x0F), (evt->param.val[0]&0x7F) |
                                                                       ((evt->param.val[1]&0x7F)<<7));
                break;
        }

        queue->pos++;
    }
}

static void FSynth_process(FSynth *self, ALuint SamplesToDo, ALfloat (*restrict DryBuffer)[BUFFERSIZE])
{
    MidiSynth *synth = STATIC_CAST(MidiSynth, self);
    ALenum state = synth->State;
    ALuint total = 0;

    if(state == AL_INITIAL)
        return;
    if(state != AL_PLAYING)
    {
        fluid_synth_write_float(self->Synth, SamplesToDo, DryBuffer[FrontLeft], 0, 1,
                                                          DryBuffer[FrontRight], 0, 1);
        return;
    }

    while(total < SamplesToDo)
    {
        if(synth->SamplesToNext >= 1.0)
        {
            ALuint todo = minu(SamplesToDo - total, fastf2u(synth->SamplesToNext));

            fluid_synth_write_float(self->Synth, todo,
                                    &DryBuffer[FrontLeft][total], 0, 1,
                                    &DryBuffer[FrontRight][total], 0, 1);
            total += todo;
            synth->SamplesSinceLast += todo;
            synth->SamplesToNext -= todo;
        }
        else
        {
            ALuint64 time = synth->NextEvtTime;
            if(time == UINT64_MAX)
            {
                synth->SamplesSinceLast += SamplesToDo-total;
                fluid_synth_write_float(self->Synth, SamplesToDo-total,
                                        &DryBuffer[FrontLeft][total], 0, 1,
                                        &DryBuffer[FrontRight][total], 0, 1);
                break;
            }

            synth->SamplesSinceLast -= (time - synth->LastEvtTime) * synth->SamplesPerTick;
            synth->SamplesSinceLast = maxd(synth->SamplesSinceLast, 0.0);
            synth->LastEvtTime = time;
            FSynth_processQueue(self, time);

            synth->NextEvtTime = MidiSynth_getNextEvtTime(synth);
            if(synth->NextEvtTime != UINT64_MAX)
                synth->SamplesToNext += (synth->NextEvtTime - synth->LastEvtTime) * synth->SamplesPerTick;
        }
    }
}


static void FSynth_Delete(FSynth *self)
{
    free(self);
}


MidiSynth *FSynth_create(ALCdevice *device)
{
    FSynth *synth = calloc(1, sizeof(*synth));
    if(!synth)
    {
        ERR("Failed to allocate FSynth\n");
        return NULL;
    }
    FSynth_Construct(synth, device);

    if(FSynth_init(synth, device) == AL_FALSE)
    {
        DELETE_OBJ(STATIC_CAST(MidiSynth, synth));
        return NULL;
    }

    return STATIC_CAST(MidiSynth, synth);
}

#else

MidiSynth *FSynth_create(ALCdevice* UNUSED(device))
{
    return NULL;
}

#endif