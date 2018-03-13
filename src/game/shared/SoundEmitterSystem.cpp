//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include <ctype.h>
#include <KeyValues.h>
#include "engine/IEngineSound.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "igamesystem.h"
#include "soundchars.h"
#include "filesystem.h"
#include "tier0/vprof.h"
#include "checksum_crc.h"
#include "tier0/icommandline.h"

#ifndef CLIENT_DLL
#include "envmicrophone.h"
#include "sceneentity.h"
#include "closedcaptions.h"
#else
#include <vgui_controls/Controls.h>
#include <vgui/IVgui.h>
#include "hud_closecaption.h"
#ifdef GAMEUI_UISYSTEM2_ENABLED
#include "gameui.h"
#endif
#define CRecipientFilter C_RecipientFilter
#endif



// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar sv_soundemitter_trace("sv_soundemitter_trace", "-1", FCVAR_REPLICATED, "Show all EmitSound calls including their symbolic name and the actual wave file they resolved to. (-1 = for nobody, 0 = for everybody, n = for one entity)\n");
ConVar cc_showmissing("cc_showmissing", "0", FCVAR_REPLICATED, "Show missing closecaption entries.");

extern ISoundEmitterSystemBase *soundemitterbase;
static ConVar *g_pClosecaption = NULL;

#ifdef _XBOX
int LookupStringFromCloseCaptionToken(char const *token);
const wchar_t *GetStringForIndex(int index);
#endif
static bool g_bPermitDirectSoundPrecache = false;

// sound scales with time
template< class T >
static FORCEINLINE T Clamp(T const &val, T const &minVal, T const &maxVal)
{
	if (val < minVal)
		return minVal;
	else if (val > maxVal)
		return maxVal;
	else
		return val;
}

#if !defined( CLIENT_DLL )

static ConVar cc_norepeat("cc_norepeat", "5", 0, "In multiplayer games, don't repeat captions more often than this many seconds.");

class CCaptionRepeatMgr
{
public:

	CCaptionRepeatMgr() :
		m_rbCaptionHistory(0, 0, DefLessFunc(unsigned int))
	{
	}

	bool CanEmitCaption(unsigned int hash);

	void Clear();

private:

	void RemoveCaptionsBefore(float t);

	struct CaptionItem_t
	{
		unsigned int	hash;
		float			realtime;

		static bool Less(const CaptionItem_t &lhs, const CaptionItem_t &rhs)
		{
			return lhs.hash < rhs.hash;
		}
	};

	CUtlMap< unsigned int, float > m_rbCaptionHistory;
};

static CCaptionRepeatMgr g_CaptionRepeats;

void CCaptionRepeatMgr::Clear()
{
	m_rbCaptionHistory.Purge();
}

bool CCaptionRepeatMgr::CanEmitCaption(unsigned int hash)
{
	// Don't cull in single player
	if (gpGlobals->maxClients == 1)
		return true;

	float realtime = gpGlobals->realtime;

	RemoveCaptionsBefore(realtime - cc_norepeat.GetFloat());

	int idx = m_rbCaptionHistory.Find(hash);
	if (idx == m_rbCaptionHistory.InvalidIndex())
	{
		m_rbCaptionHistory.Insert(hash, realtime);
		return true;
	}

	float flLastEmitted = m_rbCaptionHistory[idx];
	if (realtime - flLastEmitted > cc_norepeat.GetFloat())
	{
		m_rbCaptionHistory[idx] = realtime;
		return true;
	}

	return false;
}

void CCaptionRepeatMgr::RemoveCaptionsBefore(float t)
{
	CUtlVector< unsigned int > toRemove;
	FOR_EACH_MAP(m_rbCaptionHistory, i)
	{
		if (m_rbCaptionHistory[i] < t)
		{
			toRemove.AddToTail(m_rbCaptionHistory.Key(i));
		}
	}

	for (int i = 0; i < toRemove.Count(); ++i)
	{
		m_rbCaptionHistory.Remove(toRemove[i]);
	}
}

void ClearModelSoundsCache();

#endif // !CLIENT_DLL

void WaveTrace(char const *wavname, char const *funcname)
{
	if (IsX360() && !IsDebug())
	{
		return;
	}

	static CUtlSymbolTable s_WaveTrace;

	// Make sure we only show the message once
	if (UTL_INVAL_SYMBOL == s_WaveTrace.Find(wavname))
	{
		DevMsg("%s directly referenced wave %s (should use game_sounds.txt system instead)\n",
			funcname, wavname);
		s_WaveTrace.AddString(wavname);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : &src - 
//-----------------------------------------------------------------------------
EmitSound_t::EmitSound_t(const CSoundParameters &src)
{
	m_nChannel = src.channel;
	m_pSoundName = src.soundname;
	m_flVolume = src.volume;
	m_SoundLevel = src.soundlevel;
	m_nFlags = 0;
	m_nPitch = src.pitch;
	m_pOrigin = 0;
	m_flSoundTime = (src.delay_msec == 0) ? 0.0f : gpGlobals->curtime + ((float)src.delay_msec / 1000.0f);
	m_pflSoundDuration = 0;
	m_bEmitCloseCaption = true;
	m_bWarnOnMissingCloseCaption = false;
	m_bWarnOnDirectWaveReference = false;
	m_nSpeakerEntity = -1;
}

void Hack_FixEscapeChars(char *str)
{
	int len = Q_strlen(str) + 1;
	char *i = str;
	char *o = (char *)stackalloc(len);
	char *osave = o;
	while (*i)
	{
		if (*i == '\\')
		{
			switch (*(i + 1))
			{
			case 'n':
				*o = '\n';
				++i;
				break;
			default:
				*o = *i;
				break;
			}
		}
		else
		{
			*o = *i;
		}

		++i;
		++o;
	}
	*o = 0;
	Q_strncpy(str, osave, len);
}
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
class CSoundEmitterSystem : public CBaseGameSystem
{
public:
	virtual char const *Name() { return "CSoundEmitterSystem"; }

#if !defined( CLIENT_DLL )
	bool			m_bLogPrecache;
	FileHandle_t	m_hPrecacheLogFile;
	CUtlSymbolTable m_PrecachedScriptSounds;
	CUtlVector< AsyncCaption_t > m_ServerCaptions;

public:
	CSoundEmitterSystem(char const *pszName) :
		m_bLogPrecache(false),
		m_hPrecacheLogFile(FILESYSTEM_INVALID_HANDLE)
	{
	}

	void LogPrecache(char const *soundname)
	{
		if (!m_bLogPrecache)
			return;

		// Make sure we only show the message once
		if (UTL_INVAL_SYMBOL != m_PrecachedScriptSounds.Find(soundname))
			return;

		if (m_hPrecacheLogFile == FILESYSTEM_INVALID_HANDLE)
		{
			StartLog();
		}

		m_PrecachedScriptSounds.AddString(soundname);

		if (m_hPrecacheLogFile != FILESYSTEM_INVALID_HANDLE)
		{
			filesystem->Write("\"", 1, m_hPrecacheLogFile);
			filesystem->Write(soundname, Q_strlen(soundname), m_hPrecacheLogFile);
			filesystem->Write("\"\n", 2, m_hPrecacheLogFile);
		}
		else
		{
			Warning("Disabling precache logging due to file i/o problem!!!\n");
			m_bLogPrecache = false;
		}
	}

	void StartLog()
	{
		m_PrecachedScriptSounds.RemoveAll();

		if (!m_bLogPrecache)
			return;

		if (FILESYSTEM_INVALID_HANDLE != m_hPrecacheLogFile)
		{
			return;
		}

		filesystem->CreateDirHierarchy("reslists", "DEFAULT_WRITE_PATH");

		// open the new level reslist
		char path[_MAX_PATH];
		Q_snprintf(path, sizeof(path), "reslists\\%s.snd", gpGlobals->mapname.ToCStr());
		m_hPrecacheLogFile = filesystem->Open(path, "wt", "MOD");
		if (m_hPrecacheLogFile == FILESYSTEM_INVALID_HANDLE)
		{
			Warning("Unable to open %s for precache logging\n", path);
		}
	}

	void FinishLog()
	{
		if (FILESYSTEM_INVALID_HANDLE != m_hPrecacheLogFile)
		{
			filesystem->Close(m_hPrecacheLogFile);
			m_hPrecacheLogFile = FILESYSTEM_INVALID_HANDLE;
		}

		m_PrecachedScriptSounds.RemoveAll();
	}
#else
	CSoundEmitterSystem(char const *name)
	{
	}

#endif

	// IServerSystem stuff
	virtual bool Init()
	{
		Assert(soundemitterbase);
#if !defined( CLIENT_DLL )
		m_bLogPrecache = CommandLine()->CheckParm("-makereslists") ? true : false;
#endif
		g_pClosecaption = cvar->FindVar("closecaption");
		Assert(g_pClosecaption);

#if !defined( CLIENT_DLL )
		// Server keys off of english file!!!
		char dbfile[512];
		Q_snprintf(dbfile, sizeof(dbfile), "resource/closecaption_%s.dat", "english");

		m_ServerCaptions.Purge();

		if (IsX360())
		{
			char fullpath[MAX_PATH];
			char fullpath360[MAX_PATH];
			filesystem->RelativePathToFullPath(dbfile, "GAME", fullpath, sizeof(fullpath));
			UpdateOrCreateCaptionFile(fullpath, fullpath360, sizeof(fullpath360));
			Q_strncpy(fullpath, fullpath360, sizeof(fullpath));
		}

		int idx = m_ServerCaptions.AddToTail();
		AsyncCaption_t& entry = m_ServerCaptions[idx];
		if (!entry.LoadFromFile(dbfile))
		{
			m_ServerCaptions.Remove(idx);
		}
#endif

		return true;
	}

	virtual void Shutdown()
	{
		Assert(soundemitterbase);
#if !defined( CLIENT_DLL )
		FinishLog();
#endif
	}

	void Flush()
	{
		Shutdown();
		soundemitterbase->Flush();
#ifdef CLIENT_DLL
#ifdef GAMEUI_UISYSTEM2_ENABLED
		g_pGameUIGameSystem->ReloadSounds();
#endif
#endif
		Init();
	}

	virtual void TraceEmitSound(int originEnt, char const *fmt, ...)
	{
		if (sv_soundemitter_trace.GetInt() == -1)
			return;

		if (sv_soundemitter_trace.GetInt() != 0 && sv_soundemitter_trace.GetInt() != originEnt)
			return;

		va_list	argptr;
		char string[256];
		va_start(argptr, fmt);
		Q_vsnprintf(string, sizeof(string), fmt, argptr);
		va_end(argptr);

		// Spew to console
		Msg("%s %s", CBaseEntity::IsServer() ? "(sv)" : "(cl)", string);
	}

	// Precache all wave files referenced in wave or rndwave keys
	virtual void LevelInitPreEntity()
	{
		char mapname[256];
#if !defined( CLIENT_DLL )
		StartLog();
		Q_snprintf(mapname, sizeof(mapname), "maps/%s", STRING(gpGlobals->mapname));
#else
		Q_strncpy(mapname, engine->GetLevelName(), sizeof(mapname));
#endif

		Q_FixSlashes(mapname);
		Q_strlower(mapname);

		// Load in any map specific overrides
		char scriptfile[512];
		Q_StripExtension(mapname, scriptfile, sizeof(scriptfile));
		Q_strncat(scriptfile, "_level_sounds.txt", sizeof(scriptfile), COPY_ALL_CHARACTERS);

		if (filesystem->FileExists(scriptfile, "GAME"))
		{
			soundemitterbase->AddSoundOverrides(scriptfile);
		}

#if !defined( CLIENT_DLL )

		PreloadSounds();

		g_CaptionRepeats.Clear();
#endif
	}

	void PreloadSounds(void)
	{
		for (int i = soundemitterbase->First(); i != soundemitterbase->InvalidIndex(); i = soundemitterbase->Next(i))
		{
			CSoundParametersInternal *pParams = soundemitterbase->InternalGetParametersForSound(i);
			if (pParams->ShouldPreload())
			{
				InternalPrecacheWaves(i);
			}
		}
	}

	virtual void LevelInitPostEntity()
	{
	}

	virtual void LevelShutdownPostEntity()
	{
		soundemitterbase->ClearSoundOverrides();

#if !defined( CLIENT_DLL )
		FinishLog();

		g_CaptionRepeats.Clear();
#endif
	}

	void InternalPrecacheWaves(int soundIndex)
	{
		CSoundParametersInternal *internal = soundemitterbase->InternalGetParametersForSound(soundIndex);
		if (!internal)
			return;

		int waveCount = internal->NumSoundNames();
		if (!waveCount)
		{
			DevMsg("CSoundEmitterSystem:  sounds.txt entry '%s' has no waves listed under 'wave' or 'rndwave' key!!!\n",
				soundemitterbase->GetSoundName(soundIndex));
		}
		else
		{
			g_bPermitDirectSoundPrecache = true;

			for (int wave = 0; wave < waveCount; wave++)
			{
				CBaseEntity::PrecacheSound(soundemitterbase->GetWaveName(internal->GetSoundNames()[wave].symbol));
			}

			g_bPermitDirectSoundPrecache = false;
		}
	}

	void InternalPrefetchWaves(int soundIndex)
	{
		CSoundParametersInternal *internal = soundemitterbase->InternalGetParametersForSound(soundIndex);
		if (!internal)
			return;

		int waveCount = internal->NumSoundNames();
		if (!waveCount)
		{
			DevMsg("CSoundEmitterSystem:  sounds.txt entry '%s' has no waves listed under 'wave' or 'rndwave' key!!!\n",
				soundemitterbase->GetSoundName(soundIndex));
		}
		else
		{
			for (int wave = 0; wave < waveCount; wave++)
			{
				CBaseEntity::PrefetchSound(soundemitterbase->GetWaveName(internal->GetSoundNames()[wave].symbol));
			}
		}
	}

	HSOUNDSCRIPTHANDLE PrecacheScriptSound(const char *soundname)
	{
		int soundIndex = soundemitterbase->GetSoundIndex(soundname);
		if (!soundemitterbase->IsValidIndex(soundIndex))
		{
			if (Q_stristr(soundname, ".wav") || Q_strstr(soundname, ".mp3"))
			{
				g_bPermitDirectSoundPrecache = true;

				CBaseEntity::PrecacheSound(soundname);

				g_bPermitDirectSoundPrecache = false;
				return SOUNDEMITTER_INVALID_HANDLE;
			}

#if !defined( CLIENT_DLL )
			if (soundname[0])
			{
				static CUtlSymbolTable s_PrecacheScriptSoundFailures;

				// Make sure we only show the message once
				if (UTL_INVAL_SYMBOL == s_PrecacheScriptSoundFailures.Find(soundname))
				{
					Warning("PrecacheScriptSound '%s' failed, no such sound script entry\n", soundname);
					s_PrecacheScriptSoundFailures.AddString(soundname);
				}
			}
#endif
			return (HSOUNDSCRIPTHANDLE)soundIndex;
		}
#if !defined( CLIENT_DLL )
		LogPrecache(soundname);
#endif

		InternalPrecacheWaves(soundIndex);
		return (HSOUNDSCRIPTHANDLE)soundIndex;
	}

	void PrefetchScriptSound(const char *soundname)
	{
		int soundIndex = soundemitterbase->GetSoundIndex(soundname);
		if (!soundemitterbase->IsValidIndex(soundIndex))
		{
			if (Q_stristr(soundname, ".wav") || Q_strstr(soundname, ".mp3"))
			{
				CBaseEntity::PrefetchSound(soundname);
			}
			return;
		}

		InternalPrefetchWaves(soundIndex);
	}
public:

	void EmitSoundByHandle(IRecipientFilter& filter, int entindex, const EmitSound_t & ep, HSOUNDSCRIPTHANDLE& handle)
	{
		// Pull data from parameters
		CSoundParameters params;

		// Try to deduce the actor's gender
		gender_t gender = GENDER_NONE;
		CBaseEntity *ent = CBaseEntity::Instance(entindex);
		if (ent)
		{
			char const *actorModel = STRING(ent->GetModelName());
			gender = soundemitterbase->GetActorGender(actorModel);
		}

		if (!soundemitterbase->GetParametersForSoundEx(ep.m_pSoundName, handle, params, gender, true))
		{
			return;
		}

		if (!params.soundname[0])
			return;

		if (!Q_strncasecmp(params.soundname, "vo", 2) &&
			!(params.channel == CHAN_STREAM ||
			params.channel == CHAN_VOICE))
		{
			DevMsg("EmitSound:  Voice wave file %s doesn't specify CHAN_VOICE or CHAN_STREAM for sound %s\n",
				params.soundname, ep.m_pSoundName);
		}

		// handle SND_CHANGEPITCH/SND_CHANGEVOL and other sound flags.etc.
		if (ep.m_nFlags & SND_CHANGE_PITCH)
		{
			params.pitch = ep.m_nPitch;
		}

		if (ep.m_nFlags & SND_CHANGE_VOL)
		{
			params.volume = ep.m_flVolume;
		}

#if !defined( CLIENT_DLL )
		bool bSwallowed = CEnvMicrophone::OnSoundPlayed(
			entindex,
			params.soundname,
			params.soundlevel,
			params.volume,
			// sound scales with time
			//ep.m_nFlags, 
			//params.pitch, 
			ep.m_nFlags | SND_SHOULDPAUSE,
			Clamp(int(params.pitch * engine->GetTimescale()), 0, 255),
			ep.m_pOrigin,
			//ep.m_flSoundTime,
			ep.m_flSoundTime / engine->GetTimescale(),
			//
			ep.m_UtlVecSoundOrigin);
		if (bSwallowed)
			return;
#endif

#if defined( _DEBUG ) && !defined( CLIENT_DLL )
		if (!enginesound->IsSoundPrecached(params.soundname))
		{
			Msg("Sound %s:%s was not precached\n", ep.m_pSoundName, params.soundname);
		}
#endif
		// sound scales with time
		//float st = ep.m_flSoundTime;
		float st = ep.m_flSoundTime / engine->GetTimescale();
		if (!st &&
			params.delay_msec != 0)
		{
			st = gpGlobals->curtime + (float)params.delay_msec / 1000.f;
		}

		// TERROR:
		float startTime = Plat_FloatTime();
		enginesound->EmitSound(
			filter,
			entindex,
			params.channel,
			params.soundname,
			params.volume,
			// sound scales with time
			//(soundlevel_t)params.soundlevel,
			//ep.m_nFlags,
			//params.pitch,
			params.soundlevel,
			ep.m_nFlags | SND_SHOULDPAUSE,
			Clamp(int(params.pitch * engine->GetTimescale()), 0, 255),
			//
			ep.m_pOrigin,
			NULL,
			&ep.m_UtlVecSoundOrigin,
			true,
			st,
			ep.m_nSpeakerEntity);
		if (ep.m_pflSoundDuration)
		{
#ifdef GAME_DLL
			float startTime = Plat_FloatTime();
#endif
			*ep.m_pflSoundDuration = enginesound->GetSoundDuration(params.soundname);
#ifdef GAME_DLL
			float timeSpent = (Plat_FloatTime() - startTime) * 1000.0f;
			const float thinkLimit = 10.0f;
			if (timeSpent > thinkLimit)
			{
				UTIL_LogPrintf("getting sound duration for %s took %f milliseconds\n", params.soundname, timeSpent);
			}
#endif
		}
		// TERROR:
		float timeSpent = (Plat_FloatTime() - startTime) * 1000.0f;
		const float thinkLimit = 50.0f;
		if (timeSpent > thinkLimit)
		{
#ifdef GAME_DLL
			UTIL_LogPrintf("EmitSoundByHandle(%s) took %f milliseconds (server)\n",
				ep.m_pSoundName, timeSpent);
#else
			DevMsg("EmitSoundByHandle(%s) took %f milliseconds (client)\n",
				ep.m_pSoundName, timeSpent);
#endif
		}

		TraceEmitSound(entindex, "EmitSound:  '%s' emitted as '%s' (ent %i)\n",
			ep.m_pSoundName, params.soundname, entindex);


		// Don't caption modulations to the sound
		if (!(ep.m_nFlags & (SND_CHANGE_PITCH | SND_CHANGE_VOL)))
		{
			EmitCloseCaption(filter, entindex, params, ep);
		}
	}

	void EmitSound(IRecipientFilter& filter, int entindex, const EmitSound_t & ep)
	{
		VPROF("CSoundEmitterSystem::EmitSound (calls engine)");
		if (ep.m_pSoundName &&
			(Q_stristr(ep.m_pSoundName, ".wav") ||
			Q_stristr(ep.m_pSoundName, ".mp3") ||
			ep.m_pSoundName[0] == '!'))
		{
#if !defined( CLIENT_DLL )
			// sound scales with time
			/*bool bSwallowed = CEnvMicrophone::OnSoundPlayed(
			entindex,
			ep.m_pSoundName,
			ep.m_SoundLevel,
			ep.m_flVolume,
			ep.m_nFlags,
			ep.m_nPitch,
			ep.m_pOrigin,
			ep.m_flSoundTime,*/
			bool bSwallowed = CEnvMicrophone::OnSoundPlayed(
				entindex,
				ep.m_pSoundName,
				ep.m_SoundLevel,
				ep.m_flVolume,
				ep.m_nFlags | SND_SHOULDPAUSE,
				Clamp(int(ep.m_nPitch * engine->GetTimescale()), 0, 255),
				ep.m_pOrigin,
				ep.m_flSoundTime / engine->GetTimescale(),
				//
				ep.m_UtlVecSoundOrigin);
			if (bSwallowed)
				return;
#endif

			// TERROR:
			float startTime = Plat_FloatTime();

			if (ep.m_bWarnOnDirectWaveReference &&
				Q_stristr(ep.m_pSoundName, ".wav"))
			{
				WaveTrace(ep.m_pSoundName, "Emitsound");
			}

#if defined( _DEBUG ) && !defined( CLIENT_DLL )
			if (!enginesound->IsSoundPrecached(ep.m_pSoundName))
			{
				Msg("Sound %s was not precached\n", ep.m_pSoundName);
			}
#endif
			// sound scales with time
			/*enginesound->EmitSound(
			filter,
			entindex,
			ep.m_nChannel,
			ep.m_pSoundName,
			ep.m_flVolume,
			ep.m_SoundLevel,
			ep.m_nFlags,
			ep.m_nPitch, */
			enginesound->EmitSound(
				filter,
				entindex,
				ep.m_nChannel,
				ep.m_pSoundName,
				ep.m_flVolume,
				ep.m_SoundLevel,
				ep.m_nFlags | SND_SHOULDPAUSE,
				Clamp(int(ep.m_nPitch * engine->GetTimescale()), 0, 255),
				//
				ep.m_pOrigin,
				NULL,
				&ep.m_UtlVecSoundOrigin,
				true,
				//ep.m_flSoundTime,
				ep.m_flSoundTime / engine->GetTimescale(),
				//
				ep.m_nSpeakerEntity);
			if (ep.m_pflSoundDuration)
			{
				// TERROR:
#ifdef GAME_DLL
				UTIL_LogPrintf("getting wav duration for %s\n", ep.m_pSoundName);
#endif
				VPROF("CSoundEmitterSystem::EmitSound GetSoundDuration (calls engine)");
				// sound scales with time
				//*ep.m_pflSoundDuration = enginesound->GetSoundDuration( ep.m_pSoundName );
				*ep.m_pflSoundDuration = enginesound->GetSoundDuration(ep.m_pSoundName) / engine->GetTimescale();
			}

			TraceEmitSound(entindex, "%f EmitSound:  Raw wave emitted '%s' (ent %i) (vol %f)\n",
				gpGlobals->curtime, ep.m_pSoundName, entindex, ep.m_flVolume);

			// TERROR:
			float timeSpent = (Plat_FloatTime() - startTime) * 1000.0f;
			const float thinkLimit = 50.0f;
			if (timeSpent > thinkLimit)
			{
#ifdef GAME_DLL
				UTIL_LogPrintf("CSoundEmitterSystem::EmitSound(%s) took %f milliseconds (server)\n",
					ep.m_pSoundName, timeSpent);
#else
				DevMsg("CSoundEmitterSystem::EmitSound(%s) took %f milliseconds (client)\n",
					ep.m_pSoundName, timeSpent);
#endif
			}
			return;
		}

		if (ep.m_hSoundScriptHandle == SOUNDEMITTER_INVALID_HANDLE)
		{
			ep.m_hSoundScriptHandle = (HSOUNDSCRIPTHANDLE)soundemitterbase->GetSoundIndex(ep.m_pSoundName);
		}

		if (ep.m_hSoundScriptHandle == -1)
			return;

		EmitSoundByHandle(filter, entindex, ep, ep.m_hSoundScriptHandle);
	}

	void EmitCloseCaption(IRecipientFilter& filter, int entindex, bool fromplayer, char const *token, CUtlVector< Vector >& originlist, float duration, bool warnifmissing /*= false*/, bool bForceSubtitle = false)
	{
		// Don't use dedicated closecaption ConVar since it will prevent remote clients from getting captions.
		// Okay to use it in SP, since it's the same ConVar, not the FCVAR_USERINFO one
		if (gpGlobals->maxClients == 1 &&
			!g_pClosecaption->GetBool())
		{
			return;
		}

		// A negative duration means fill it in from the wav file if possible
		if (duration < 0.0f)
		{
			char const *wav = soundemitterbase->GetWavFileForSound(token, GENDER_NONE);
			if (wav)
			{
				// sound scales with time
				//duration = enginesound->GetSoundDuration( wav );
				duration = enginesound->GetSoundDuration(wav) / engine->GetTimescale();
			}
			else
			{
				duration = 2.0f;
			}
		}

		char lowercase[256];
		Q_strncpy(lowercase, token, sizeof(lowercase));
		Q_strlower(lowercase);
		if (Q_strstr(lowercase, "\\"))
		{
			Hack_FixEscapeChars(lowercase);
		}

		// NOTE:  We must make a copy or else if the filter is owned by a SoundPatch, we'll end up destructively removing
		//  all players from it!!!!
		CRecipientFilter filterCopy;
		filterCopy.CopyFrom((CRecipientFilter &)filter);

		// Captions only route to host player (there is only one closecaptioning HUD)
		filterCopy.RemoveSplitScreenPlayers();

		if (!bForceSubtitle)
		{
			// Remove any players who don't want close captions
			CBaseEntity::RemoveRecipientsIfNotCloseCaptioning((CRecipientFilter &)filterCopy);
		}

#if !defined( CLIENT_DLL )
		{
			// Defined in sceneentity.cpp
			bool AttenuateCaption(const char *token, const Vector& listener, CUtlVector< Vector >& soundorigins);

			if (filterCopy.GetRecipientCount() > 0)
			{
				int c = filterCopy.GetRecipientCount();
				for (int i = c - 1; i >= 0; --i)
				{
					CBasePlayer *player = UTIL_PlayerByIndex(filterCopy.GetRecipientIndex(i));
					if (!player)
						continue;

					Vector playerEarPosition = player->EarPosition();

					if (AttenuateCaption(lowercase, playerEarPosition, originlist))
					{
						filterCopy.RemoveRecipient(player);
					}
				}
			}
		}
#endif
		// Anyone left?
		if (filterCopy.GetRecipientCount() > 0)
		{

#if !defined( CLIENT_DLL )

			char lowercase_nogender[256];
			Q_strncpy(lowercase_nogender, lowercase, sizeof(lowercase_nogender));
			bool bTriedGender = false;

			CBaseEntity *pActor = CBaseEntity::Instance(entindex);
			if (pActor)
			{
				char const *pszActorModel = STRING(pActor->GetModelName());
				gender_t gender = soundemitterbase->GetActorGender(pszActorModel);

				if (gender == GENDER_MALE)
				{
					Q_strncat(lowercase, "_male", sizeof(lowercase), COPY_ALL_CHARACTERS);
					bTriedGender = true;
				}
				else if (gender == GENDER_FEMALE)
				{
					Q_strncat(lowercase, "_female", sizeof(lowercase), COPY_ALL_CHARACTERS);
					bTriedGender = true;
				}
			}

			unsigned int hash = 0u;
			bool bFound = GetCaptionHash(lowercase, true, hash);

			// if not found, try the no-gender version
			if (!bFound && bTriedGender)
			{
				bFound = GetCaptionHash(lowercase_nogender, true, hash);
			}

			if (bFound)
			{
				if (g_CaptionRepeats.CanEmitCaption(hash))
				{
					if (bForceSubtitle)
					{
						// Send forced caption and duration hint down to client
						UserMessageBegin(filterCopy, "CloseCaptionDirect");
						WRITE_LONG(hash);
						WRITE_UBITLONG(clamp((int)(duration * 10.0f), 0, 65535), 15),
							WRITE_UBITLONG(fromplayer ? 1 : 0, 1),
							MessageEnd();
					}
					else
					{
						// Send caption and duration hint down to client
						UserMessageBegin(filterCopy, "CloseCaption");
						WRITE_LONG(hash);
						WRITE_UBITLONG(clamp((int)(duration * 10.0f), 0, 65535), 15),
							WRITE_UBITLONG(fromplayer ? 1 : 0, 1),
							MessageEnd();
					}
				}
			}
#else
			// Direct dispatch
			CHudCloseCaption *cchud = GET_FULLSCREEN_HUDELEMENT(CHudCloseCaption);
			if (cchud)
			{
				cchud->ProcessCaption(lowercase, duration, fromplayer);
			}
#endif
		}
	}

	void EmitCloseCaption(IRecipientFilter& filter, int entindex, const CSoundParameters & params, const EmitSound_t & ep)
	{
		// Don't use dedicated closecaption ConVar since it will prevent remote clients from getting captions.
		// Okay to use it in SP, since it's the same ConVar, not the FCVAR_USERINFO one
		if (gpGlobals->maxClients == 1 &&
			!g_pClosecaption->GetBool())
		{
			return;
		}

		bool bForceSubtitle = false;

		if (TestSoundChar(params.soundname, CHAR_SUBTITLED))
		{
			bForceSubtitle = true;
		}

		if (!bForceSubtitle && !ep.m_bEmitCloseCaption)
		{
			return;
		}

		// NOTE:  We must make a copy or else if the filter is owned by a SoundPatch, we'll end up destructively removing
		//  all players from it!!!!
		CRecipientFilter filterCopy;
		filterCopy.CopyFrom((CRecipientFilter &)filter);

		if (!bForceSubtitle)
		{
			// Remove any players who don't want close captions
			CBaseEntity::RemoveRecipientsIfNotCloseCaptioning((CRecipientFilter &)filterCopy);
		}

		// Anyone left?
		if (filterCopy.GetRecipientCount() <= 0)
		{
			return;
		}

		float duration = 0.0f;
		if (ep.m_pflSoundDuration)
		{
			duration = *ep.m_pflSoundDuration;
		}
		else
		{
			duration = enginesound->GetSoundDuration(params.soundname);
		}

		bool fromplayer = false;
		CBaseEntity *ent = CBaseEntity::Instance(entindex);
		if (ent)
		{
			while (ent)
			{
				if (ent->IsPlayer())
				{
					fromplayer = true;
					break;
				}

				ent = ent->GetOwnerEntity();
			}
		}
		EmitCloseCaption(filter, entindex, fromplayer, ep.m_pSoundName, ep.m_UtlVecSoundOrigin, duration, ep.m_bWarnOnMissingCloseCaption, bForceSubtitle);
	}

	void EmitAmbientSound(int entindex, const Vector& origin, const char *soundname, float flVolume, int iFlags, int iPitch, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
	{
		// Pull data from parameters
		CSoundParameters params;

		if (!soundemitterbase->GetParametersForSound(soundname, params, GENDER_NONE))
		{
			return;
		}

		if (iFlags & SND_CHANGE_PITCH)
		{
			params.pitch = iPitch;
		}

		if (iFlags & SND_CHANGE_VOL)
		{
			params.volume = flVolume;
		}

#if defined( CLIENT_DLL )
		// sound scales with time
		//enginesound->EmitAmbientSound( params.soundname, params.volume, params.pitch, iFlags, soundtime );
		enginesound->EmitAmbientSound(params.soundname, params.volume, Clamp(int(params.pitch * engine->GetTimescale()), 0, 255), iFlags | SND_SHOULDPAUSE, soundtime / engine->GetTimescale());
#else
		//engine->EmitAmbientSound(entindex, origin, params.soundname, params.volume, params.soundlevel, iFlags, params.pitch, soundtime );
		engine->EmitAmbientSound(entindex, origin, params.soundname, params.volume, params.soundlevel, iFlags | SND_SHOULDPAUSE, Clamp(int(params.pitch * engine->GetTimescale()), 0, 255), soundtime / engine->GetTimescale());
#endif

		bool needsCC = !(iFlags & (SND_STOP | SND_CHANGE_VOL | SND_CHANGE_PITCH));

		float soundduration = 0.0f;

		if (duration || needsCC)
		{
			// sound scales with time
			//soundduration = enginesound->GetSoundDuration( params.soundname );
			soundduration = enginesound->GetSoundDuration(params.soundname) / engine->GetTimescale();
			if (duration)
			{
				*duration = soundduration;
			}
		}

		TraceEmitSound(entindex, "EmitAmbientSound:  '%s' emitted as '%s' (ent %i)\n",
			soundname, params.soundname, entindex);

		// We only want to trigger the CC on the start of the sound, not on any changes or halting of the sound
		if (needsCC)
		{
			CRecipientFilter filter;
			filter.AddAllPlayers();
			filter.MakeReliable();

			CUtlVector< Vector > dummy;
			EmitCloseCaption(filter, entindex, false, soundname, dummy, soundduration, false);
		}

	}

	void StopSoundByHandle(int entindex, const char *soundname, HSOUNDSCRIPTHANDLE& handle, bool bIsStoppingSpeakerSound = false)
	{
		if (handle == SOUNDEMITTER_INVALID_HANDLE)
		{
			handle = (HSOUNDSCRIPTHANDLE)soundemitterbase->GetSoundIndex(soundname);
		}

		if (handle == SOUNDEMITTER_INVALID_HANDLE)
			return;

		CSoundParametersInternal *params;

		params = soundemitterbase->InternalGetParametersForSound((int)handle);
		if (!params)
		{
			return;
		}

		// HACK:  we have to stop all sounds if there are > 1 in the rndwave section...
		int c = params->NumSoundNames();
		for (int i = 0; i < c; ++i)
		{
			char const *wavename = soundemitterbase->GetWaveName(params->GetSoundNames()[i].symbol);
			Assert(wavename);

			enginesound->StopSound(
				entindex,
				params->GetChannel(),
				wavename);

			TraceEmitSound(entindex, "StopSound:  '%s' stopped as '%s' (ent %i)\n",
				soundname, wavename, entindex);

#if !defined ( CLIENT_DLL )
			if (bIsStoppingSpeakerSound == false)
			{
				StopSpeakerSounds(wavename);
			}
#endif // !CLIENT_DLL 
		}

	}

	void StopSound(int entindex, const char *soundname)
	{
		HSOUNDSCRIPTHANDLE handle = (HSOUNDSCRIPTHANDLE)soundemitterbase->GetSoundIndex(soundname);
		if (handle == SOUNDEMITTER_INVALID_HANDLE)
		{
			return;
		}

		StopSoundByHandle(entindex, soundname, handle);
	}


	void StopSound(int iEntIndex, int iChannel, const char *pSample, bool bIsStoppingSpeakerSound = false)
	{
		if (pSample && (Q_stristr(pSample, ".wav") || Q_stristr(pSample, ".mp3") || pSample[0] == '!'))
		{
			enginesound->StopSound(iEntIndex, iChannel, pSample);

			TraceEmitSound(iEntIndex, "StopSound:  Raw wave stopped '%s' (ent %i)\n",
				pSample, iEntIndex);
#if !defined ( CLIENT_DLL )
			if (bIsStoppingSpeakerSound == false)
			{
				StopSpeakerSounds(pSample);
			}
#endif // !CLIENT_DLL 
		}
		else
		{
			// Look it up in sounds.txt and ignore other parameters
			StopSound(iEntIndex, pSample);
		}
	}

	void EmitAmbientSound(int entindex, const Vector &origin, const char *pSample, float volume, soundlevel_t soundlevel, int flags, int pitch, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
	{
#if !defined( CLIENT_DLL )
		CUtlVector< Vector > dummyorigins;

		// Loop through all registered microphones and tell them the sound was just played
		// NOTE: This means that pitch shifts/sound changes on the original ambient will not be reflected in the re-broadcasted sound
		bool bSwallowed = CEnvMicrophone::OnSoundPlayed(
			entindex,
			pSample,
			soundlevel,
			volume,
			// sound scales with time
			//flags, 
			//pitch, 
			flags | SND_SHOULDPAUSE,
			Clamp(int(pitch * engine->GetTimescale()), 0, 255),
			&origin,
			//soundtime,
			soundtime / engine->GetTimescale(),
			dummyorigins);
		if (bSwallowed)
			return;
#endif

		if (pSample && (Q_stristr(pSample, ".wav") || Q_stristr(pSample, ".mp3")))
		{
#if defined( CLIENT_DLL )
			// sound scales with time
			//enginesound->EmitAmbientSound( pSample, volume, pitch, flags, soundtime );
			enginesound->EmitAmbientSound(pSample, volume, Clamp(int(pitch * engine->GetTimescale()), 0, 255), flags | SND_SHOULDPAUSE, soundtime / engine->GetTimescale());
#else
			//engine->EmitAmbientSound( entindex, origin, pSample, volume, soundlevel, flags, pitch, soundtime );
			engine->EmitAmbientSound(entindex, origin, pSample, volume, soundlevel, flags | SND_SHOULDPAUSE, Clamp(int(pitch * engine->GetTimescale()), 0, 255), soundtime / engine->GetTimescale());
#endif

			if (duration)
			{
				// sound scales with time
				//*duration = enginesound->GetSoundDuration( pSample );
				*duration = enginesound->GetSoundDuration(pSample) / engine->GetTimescale();
			}

			TraceEmitSound(entindex, "EmitAmbientSound:  Raw wave emitted '%s' (ent %i)\n",
				pSample, entindex);
		}
		else
		{
			EmitAmbientSound(entindex, origin, pSample, volume, flags, pitch, soundtime, duration);
		}
	}


#if !defined( CLIENT_DLL )
	bool GetCaptionHash(char const *pchStringName, bool bWarnIfMissing, unsigned int &hash)
	{
		// hash the string, find in dictionary or return 0u if not there!!!
		CUtlVector< AsyncCaption_t >& directories = m_ServerCaptions;

		CaptionLookup_t search;
		search.SetHash(pchStringName);
		hash = search.hash;

		int idx = -1;
		int i;
		int dc = directories.Count();
		for (i = 0; i < dc; ++i)
		{
			idx = directories[i].m_CaptionDirectory.Find(search);
			if (idx == directories[i].m_CaptionDirectory.InvalidIndex())
				continue;

			break;
		}

		if (i >= dc || idx == -1)
		{
			if (bWarnIfMissing && cc_showmissing.GetBool())
			{
				static CUtlRBTree< unsigned int > s_MissingHashes(0, 0, DefLessFunc(unsigned int));
				if (s_MissingHashes.Find(hash) == s_MissingHashes.InvalidIndex())
				{
					s_MissingHashes.Insert(hash);
					Msg("Missing caption for %s\n", pchStringName);
				}
			}
			return false;
		}

		// Anything marked as L"" by content folks doesn't need to transmit either!!!
		CaptionLookup_t &entry = directories[i].m_CaptionDirectory[idx];
		if (entry.length <= sizeof(wchar_t))
		{
			return false;
		}

		return true;
	}
	void StopSpeakerSounds(const char *wavename)
	{
		// Stop sound on any speakers playing this wav name
		// but don't recurse in if this stopsound is happening on a speaker
		CEnvMicrophone::OnSoundStopped(wavename);
	}
#endif
};

static CSoundEmitterSystem g_SoundEmitterSystem("CSoundEmitterSystem");

IGameSystem *SoundEmitterSystem()
{
	return &g_SoundEmitterSystem;
}

void SoundSystemPreloadSounds(void)
{
	g_SoundEmitterSystem.PreloadSounds();
}

#if !defined( CLIENT_DLL )

CON_COMMAND(sv_soundemitter_flush, "Flushes the sounds.txt system (server only)")
{
	if (!UTIL_IsCommandIssuedByServerAdmin())
		return;

	// save the current soundscape
	// kill the system
	g_SoundEmitterSystem.Flush();

	// Redo precache all wave files... (this should work now that we have dynamic string tables)
	g_SoundEmitterSystem.LevelInitPreEntity();

	// These store raw sound indices for faster precaching, blow them away.
	ClearModelSoundsCache();
	// TODO:  when we go to a handle system, we'll need to invalidate handles somehow
}

CON_COMMAND(sv_soundemitter_filecheck, "Report missing wave files for sounds and game_sounds files.")
{
	if (!UTIL_IsCommandIssuedByServerAdmin())
		return;

	int missing = soundemitterbase->CheckForMissingWavFiles(true);
	DevMsg("---------------------------\nTotal missing files %i\n", missing);
}

CON_COMMAND(sv_findsoundname, "Find sound names which reference the specified wave files.")
{
	if (!UTIL_IsCommandIssuedByServerAdmin())
		return;

	if (args.ArgC() != 2)
		return;

	int c = soundemitterbase->GetSoundCount();
	int i;

	char const *search = args[1];
	if (!search)
		return;

	for (i = 0; i < c; i++)
	{
		CSoundParametersInternal *internal = soundemitterbase->InternalGetParametersForSound(i);
		if (!internal)
			continue;

		int waveCount = internal->NumSoundNames();
		if (waveCount > 0)
		{
			for (int wave = 0; wave < waveCount; wave++)
			{
				char const *wavefilename = soundemitterbase->GetWaveName(internal->GetSoundNames()[wave].symbol);

				if (Q_stristr(wavefilename, search))
				{
					char const *soundname = soundemitterbase->GetSoundName(i);
					char const *scriptname = soundemitterbase->GetSourceFileForSound(i);

					Msg("Referenced by '%s:%s' -- %s\n", scriptname, soundname, wavefilename);
				}
			}
		}
	}
}

CON_COMMAND(sv_soundemitter_spew, "Print details about a sound.")
{
	if (args.ArgC() != 2)
	{
		Msg("Usage:  soundemitter_spew < sndname >\n");
		return;
	}

	soundemitterbase->DescribeSound(args.Arg(1));
}

#else

void Playgamesound_f(const CCommand &args)
{
	CBasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if (pPlayer)
	{
		if (args.ArgC() > 2)
		{
			Vector position = pPlayer->EyePosition();
			Vector forward;
			pPlayer->GetVectors(&forward, NULL, NULL);
			position += atof(args[2]) * forward;
			ABS_QUERY_GUARD(true);
			CPASAttenuationFilter filter(pPlayer);
			EmitSound_t params;
			params.m_pSoundName = args[1];
			params.m_pOrigin = &position;
			params.m_flVolume = 0.0f;
			params.m_nPitch = 0;
			g_SoundEmitterSystem.EmitSound(filter, 0, params);
		}
		else
		{
			pPlayer->EmitSound(args[1]);
		}
	}
	else
	{
		Msg("Can't play until a game is started.\n");
		// UNDONE: Make something like this work?
		//CBroadcastRecipientFilter filter;
		//g_SoundEmitterSystem.EmitSound( filter, 1, args[1], 0.0, 0, 0, &vec3_origin, 0, NULL );
	}
}

static int GamesoundCompletion(const char *partial, char commands[COMMAND_COMPLETION_MAXITEMS][COMMAND_COMPLETION_ITEM_LENGTH])
{
	int current = 0;

	const char *cmdname = "playgamesound";
	char *substring = NULL;
	int substringLen = 0;
	if (Q_strstr(partial, cmdname) && strlen(partial) > strlen(cmdname) + 1)
	{
		substring = (char *)partial + strlen(cmdname) + 1;
		substringLen = strlen(substring);
	}

	for (int i = soundemitterbase->GetSoundCount() - 1; i >= 0 && current < COMMAND_COMPLETION_MAXITEMS; i--)
	{
		const char *pSoundName = soundemitterbase->GetSoundName(i);
		if (pSoundName)
		{
			if (!substring || !Q_strncasecmp(pSoundName, substring, substringLen))
			{
				Q_snprintf(commands[current], sizeof(commands[current]), "%s %s", cmdname, pSoundName);
				current++;
			}
		}
	}

	return current;
}

static ConCommand Command_Playgamesound("playgamesound", Playgamesound_f, "Play a sound from the game sounds txt file", FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE, GamesoundCompletion);




// --------------------------------------------------------------------
// snd_playsounds
//
// This a utility for testing sound values
// --------------------------------------------------------------------

static int GamesoundCompletion2(const char *partial, char commands[COMMAND_COMPLETION_MAXITEMS][COMMAND_COMPLETION_ITEM_LENGTH])
{
	int current = 0;

	const char *cmdname = "snd_playsounds";
	char *substring = NULL;
	int substringLen = 0;
	if (Q_strstr(partial, cmdname) && strlen(partial) > strlen(cmdname) + 1)
	{
		substring = (char *)partial + strlen(cmdname) + 1;
		substringLen = strlen(substring);
	}

	for (int i = soundemitterbase->GetSoundCount() - 1; i >= 0 && current < COMMAND_COMPLETION_MAXITEMS; i--)
	{
		const char *pSoundName = soundemitterbase->GetSoundName(i);
		if (pSoundName)
		{
			if (!substring || !Q_strncasecmp(pSoundName, substring, substringLen))
			{
				Q_snprintf(commands[current], sizeof(commands[current]), "%s %s", cmdname, pSoundName);
				current++;
			}
		}
	}

	return current;
}

void S_PlaySounds(const CCommand &args)
{
	CBasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if (pPlayer)
	{
		if (args.ArgC() > 4)
		{
			//			Vector position = pPlayer->EyePosition();
			Vector position;
			//	Vector forward;
			//	pPlayer->GetVectors( &forward, NULL, NULL );
			//	position += atof( args[2] ) * forward;
			position[0] = atof(args[2]);
			position[1] = atof(args[3]);
			position[2] = atof(args[4]);

			ABS_QUERY_GUARD(true);
			CPASAttenuationFilter filter(pPlayer);
			EmitSound_t params;
			params.m_pSoundName = args[1];
			params.m_pOrigin = &position;
			params.m_flVolume = 0.0f;
			params.m_nPitch = 0;
			g_SoundEmitterSystem.EmitSound(filter, 0, params);
		}
		else
		{
			pPlayer->EmitSound(args[1]);
		}
	}
	else
	{
		Msg("Can't play until a game is started.\n");
		// UNDONE: Make something like this work?
		//CBroadcastRecipientFilter filter;
		//g_SoundEmitterSystem.EmitSound( filter, 1, args[1], 0.0, 0, 0, &vec3_origin, 0, NULL );
	}
}


static ConCommand SND_PlaySounds("snd_playsounds", S_PlaySounds, "Play sounds from the game sounds txt file at a given location", FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE, GamesoundCompletion2);

static int GamesoundCompletion3(const char *partial, char commands[COMMAND_COMPLETION_MAXITEMS][COMMAND_COMPLETION_ITEM_LENGTH])
{
	int current = 0;

	const char *cmdname = "snd_setsoundparam";
	char *substring = NULL;
	int substringLen = 0;
	if (Q_strstr(partial, cmdname) && strlen(partial) > strlen(cmdname) + 1)
	{
		substring = (char *)partial + strlen(cmdname) + 1;
		substringLen = strlen(substring);
	}

	for (int i = soundemitterbase->GetSoundCount() - 1; i >= 0 && current < COMMAND_COMPLETION_MAXITEMS; i--)
	{
		const char *pSoundName = soundemitterbase->GetSoundName(i);
		if (pSoundName)
		{
			if (!substring || !Q_strncasecmp(pSoundName, substring, substringLen))
			{
				Q_snprintf(commands[current], sizeof(commands[current]), "%s %s", cmdname, pSoundName);
				current++;
			}
		}
	}

	return current;
}

static void S_SetSoundParam(const CCommand &args)
{
	if (args.ArgC() != 4)
	{
		DevMsg("Parameters: mix group name, [vol, mute, solo], value");
		return;
	}

	const char *szSoundName = args[1];
	const char *szparam = args[2];
	const char *szValue = args[3];

	// get the sound we're working on
	int soundindex = soundemitterbase->GetSoundIndex(szSoundName);
	if (!soundemitterbase->IsValidIndex(soundindex))
		return;

	// Look up the sound level from the soundemitter system
	CSoundParametersInternal *soundparams = soundemitterbase->InternalGetParametersForSound(soundindex);
	if (!soundparams)
	{
		return;
	}

	// // See if it's writable, if not then bail
	// char const *scriptfile = soundemitter->GetSourceFileForSound( soundindex );
	// if ( !scriptfile || 
	// !filesystem->FileExists( scriptfile ) ||
	// !filesystem->IsFileWritable( scriptfile ) )
	// {
	// return;
	// }

	// Copy the parameters
	CSoundParametersInternal newparams;
	newparams.CopyFrom(*soundparams);

	if (!Q_stricmp("volume", szparam))
		newparams.VolumeFromString(szValue);
	else if (!Q_stricmp("level", szparam))
		newparams.SoundLevelFromString(szValue);

	// No change
	if (newparams == *soundparams)
	{
		return;
	}

	soundemitterbase->UpdateSoundParameters(szSoundName, newparams);

}

static ConCommand SND_SetSoundParam("snd_setsoundparam", S_SetSoundParam, "Set a sound paramater", FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE, GamesoundCompletion3);

#endif // CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose:  Non-static override for doing the general case of CPASAttenuationFilter( this ), and EmitSound( filter, entindex(), etc. );
// Input  : *soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(const char *soundname, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	//VPROF( "CBaseEntity::EmitSound" );
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	ABS_QUERY_GUARD(true);
	CPASAttenuationFilter filter(this, soundname);
	EmitSound_t params;
	params.m_pSoundName = soundname;
	params.m_flSoundTime = soundtime;
	params.m_pflSoundDuration = duration;
	params.m_bWarnOnDirectWaveReference = true;

	EmitSound(filter, entindex(), params);
}

//-----------------------------------------------------------------------------
// Purpose:  Non-static override for doing the general case of CPASAttenuationFilter( this ), and EmitSound( filter, entindex(), etc. );
// Input  : *soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(const char *soundname, HSOUNDSCRIPTHANDLE& handle, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	// VPROF( "CBaseEntity::EmitSound" );
	ABS_QUERY_GUARD(true);
	CPASAttenuationFilter filter(this, soundname, handle);

	EmitSound_t params;
	params.m_pSoundName = soundname;
	params.m_flSoundTime = soundtime;
	params.m_pflSoundDuration = duration;
	params.m_bWarnOnDirectWaveReference = true;

	EmitSound(filter, entindex(), params, handle);
}

#if !defined ( CLIENT_DLL )
void CBaseEntity::ScriptEmitSound(const char *soundname)
{
	EmitSound(soundname);
}

float CBaseEntity::ScriptSoundDuration(const char *soundname, const char *actormodel)
{
	float duration = CBaseEntity::GetSoundDuration(soundname, actormodel);
	return duration;
}
#endif // !CLIENT

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : filter - 
//			iEntIndex - 
//			*soundname - 
//			*pOrigin - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(IRecipientFilter& filter, int iEntIndex, const char *soundname, const Vector *pOrigin /*= NULL*/, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	// VPROF( "CBaseEntity::EmitSound" );
	EmitSound_t params;
	params.m_pSoundName = soundname;
	params.m_flSoundTime = soundtime;
	params.m_pOrigin = pOrigin;
	params.m_pflSoundDuration = duration;
	params.m_bWarnOnDirectWaveReference = true;

	EmitSound(filter, iEntIndex, params, params.m_hSoundScriptHandle);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : filter - 
//			iEntIndex - 
//			*soundname - 
//			*pOrigin - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(IRecipientFilter& filter, int iEntIndex, const char *soundname, HSOUNDSCRIPTHANDLE& handle, const Vector *pOrigin /*= NULL*/, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	//VPROF( "CBaseEntity::EmitSound" );
	EmitSound_t params;
	params.m_pSoundName = soundname;
	params.m_flSoundTime = soundtime;
	params.m_pOrigin = pOrigin;
	params.m_pflSoundDuration = duration;
	params.m_bWarnOnDirectWaveReference = true;

	EmitSound(filter, iEntIndex, params, handle);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : filter - 
//			iEntIndex - 
//			params - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(IRecipientFilter& filter, int iEntIndex, const EmitSound_t & params)
{
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	// VPROF( "CBaseEntity::EmitSound" );
	// Call into the sound emitter system...
	g_SoundEmitterSystem.EmitSound(filter, iEntIndex, params);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : filter - 
//			iEntIndex - 
//			params - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitSound(IRecipientFilter& filter, int iEntIndex, const EmitSound_t & params, HSOUNDSCRIPTHANDLE& handle)
{
	VPROF_BUDGET("CBaseEntity::EmitSound", _T("CBaseEntity::EmitSound"));

	// VPROF( "CBaseEntity::EmitSound" );
	// Call into the sound emitter system...
	g_SoundEmitterSystem.EmitSoundByHandle(filter, iEntIndex, params, handle);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::StopSound(const char *soundname)
{
#if defined( CLIENT_DLL )
	if (entindex() == -1)
	{
		// If we're a clientside entity, we need to use the soundsourceindex instead of the entindex
		StopSound(GetSoundSourceIndex(), soundname);
		return;
	}
#endif

	StopSound(entindex(), soundname);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::StopSound(const char *soundname, HSOUNDSCRIPTHANDLE& handle)
{
#if defined( CLIENT_DLL )
	if (entindex() == -1)
	{
		// If we're a clientside entity, we need to use the soundsourceindex instead of the entindex
		StopSound(GetSoundSourceIndex(), soundname);
		return;
	}
#endif

	g_SoundEmitterSystem.StopSoundByHandle(entindex(), soundname, handle);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : iEntIndex - 
//			*soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::StopSound(int iEntIndex, const char *soundname)
{
	g_SoundEmitterSystem.StopSound(iEntIndex, soundname);
}

void CBaseEntity::StopSound(int iEntIndex, int iChannel, const char *pSample, bool bIsStoppingSpeakerSound)
{
	g_SoundEmitterSystem.StopSound(iEntIndex, iChannel, pSample, bIsStoppingSpeakerSound);
}

soundlevel_t CBaseEntity::LookupSoundLevel(const char *soundname)
{
	return soundemitterbase->LookupSoundLevel(soundname);
}


soundlevel_t CBaseEntity::LookupSoundLevel(const char *soundname, HSOUNDSCRIPTHANDLE& handle)
{
	return soundemitterbase->LookupSoundLevelByHandle(soundname, handle);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *entity - 
//			origin - 
//			flags - 
//			*soundname - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitAmbientSound(int entindex, const Vector& origin, const char *soundname, int flags, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	g_SoundEmitterSystem.EmitAmbientSound(entindex, origin, soundname, 0.0, flags, 0, soundtime, duration);
}

// HACK HACK:  Do we need to pull the entire SENTENCEG_* wrapper over to the client .dll?
#if defined( CLIENT_DLL )
int SENTENCEG_Lookup(const char *sample)
{
	return engine->SentenceIndexFromName(sample + 1);
}
#endif

void UTIL_EmitAmbientSound(int entindex, const Vector &vecOrigin, const char *samp, float vol, soundlevel_t soundlevel, int fFlags, int pitch, float soundtime /*= 0.0f*/, float *duration /*=NULL*/)
{
	if (samp && *samp == '!')
	{
		int sentenceIndex = SENTENCEG_Lookup(samp);
		if (sentenceIndex >= 0)
		{
			char name[32];
			Q_snprintf(name, sizeof(name), "!%d", sentenceIndex);
#if !defined( CLIENT_DLL )
			// sound scales with time
			//engine->EmitAmbientSound( entindex, vecOrigin, name, vol, soundlevel, fFlags, pitch, soundtime );
			engine->EmitAmbientSound(entindex, vecOrigin, name, vol, soundlevel, fFlags | SND_SHOULDPAUSE, Clamp(int(pitch * engine->GetTimescale()), 0, 255), soundtime / engine->GetTimescale());
#else
			//enginesound->EmitAmbientSound( name, vol, pitch, fFlags, soundtime );
			enginesound->EmitAmbientSound(name, vol, Clamp(int(pitch * engine->GetTimescale()), 0, 255), fFlags | SND_SHOULDPAUSE, soundtime / engine->GetTimescale());
#endif
			if (duration)
			{
				//*duration = enginesound->GetSoundDuration( name );
				*duration = enginesound->GetSoundDuration(name) / engine->GetTimescale();
			}

			g_SoundEmitterSystem.TraceEmitSound(entindex, "UTIL_EmitAmbientSound:  Sentence emitted '%s' (ent %i)\n",
				name, entindex);
		}
	}
	else
	{
		g_SoundEmitterSystem.EmitAmbientSound(entindex, vecOrigin, samp, vol, soundlevel, fFlags, pitch, soundtime, duration);
	}
}

static const char *UTIL_TranslateSoundName(const char *soundname, const char *actormodel)
{
	Assert(soundname);

	if (Q_stristr(soundname, ".wav") || Q_stristr(soundname, ".mp3"))
	{
		if (Q_stristr(soundname, ".wav"))
		{
			WaveTrace(soundname, "UTIL_TranslateSoundName");
		}
		return soundname;
	}

	return soundemitterbase->GetWavFileForSound(soundname, actormodel);
}

void CBaseEntity::GenderExpandString(char const *in, char *out, int maxlen)
{
	soundemitterbase->GenderExpandString(STRING(GetModelName()), in, out, maxlen);
}

bool CBaseEntity::GetParametersForSound(const char *soundname, CSoundParameters &params, const char *actormodel)
{
	gender_t gender = soundemitterbase->GetActorGender(actormodel);

	return soundemitterbase->GetParametersForSound(soundname, params, gender);
}

bool CBaseEntity::GetParametersForSound(const char *soundname, HSOUNDSCRIPTHANDLE& handle, CSoundParameters &params, const char *actormodel)
{
	gender_t gender = soundemitterbase->GetActorGender(actormodel);

	return soundemitterbase->GetParametersForSoundEx(soundname, handle, params, gender);
}

HSOUNDSCRIPTHANDLE CBaseEntity::PrecacheScriptSound(const char *soundname)
{
#if !defined( CLIENT_DLL )
	return g_SoundEmitterSystem.PrecacheScriptSound(soundname);
#else
	return soundemitterbase->GetSoundIndex(soundname);
#endif
}

#if !defined ( CLIENT_DLL )
// Same as server version of above, but signiture changed so it can be deduced by the macros
void CBaseEntity::VScriptPrecacheScriptSound(const char *soundname)
{
	g_SoundEmitterSystem.PrecacheScriptSound(soundname);
}
#endif // !CLIENT_DLL

void CBaseEntity::PrefetchScriptSound(const char *soundname)
{
	g_SoundEmitterSystem.PrefetchScriptSound(soundname);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *soundname - 
// Output : float
//-----------------------------------------------------------------------------
float CBaseEntity::GetSoundDuration(const char *soundname, char const *actormodel)
{
	return enginesound->GetSoundDuration(PSkipSoundChars(UTIL_TranslateSoundName(soundname, actormodel)));
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : filter - 
//			*token - 
//			duration - 
//			warnifmissing - 
//-----------------------------------------------------------------------------
void CBaseEntity::EmitCloseCaption(IRecipientFilter& filter, int entindex, char const *token, CUtlVector< Vector >& soundorigin, float duration, bool warnifmissing /*= false*/)
{
	bool fromplayer = false;
	CBaseEntity *ent = CBaseEntity::Instance(entindex);
	while (ent)
	{
		if (ent->IsPlayer())
		{
			fromplayer = true;
			break;
		}
		ent = ent->GetOwnerEntity();
	}

	g_SoundEmitterSystem.EmitCloseCaption(filter, entindex, fromplayer, token, soundorigin, duration, warnifmissing);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
//			preload - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CBaseEntity::PrecacheSound(const char *name)
{
	if (IsPC() && !g_bPermitDirectSoundPrecache)
	{
		Warning("Direct precache of %s\n", name);
	}

	// If this is out of order, warn
	if (!CBaseEntity::IsPrecacheAllowed())
	{
		if (!enginesound->IsSoundPrecached(name))
		{
			Assert(!"CBaseEntity::PrecacheSound:  too late");

			Warning("Late precache of %s\n", name);
		}
	}

	bool bret = enginesound->PrecacheSound(name, true);
	return bret;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
//-----------------------------------------------------------------------------
void CBaseEntity::PrefetchSound(const char *name)
{
	enginesound->PrefetchSound(name);
}

#if !defined( CLIENT_DLL )
bool GetCaptionHash(char const *pchStringName, bool bWarnIfMissing, unsigned int &hash)
{
	return g_SoundEmitterSystem.GetCaptionHash(pchStringName, bWarnIfMissing, hash);
}

bool CanEmitCaption(unsigned int hash)
{
	return g_CaptionRepeats.CanEmitCaption(hash);
}

#endif