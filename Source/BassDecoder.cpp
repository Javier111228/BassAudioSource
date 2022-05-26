/*
 *  Copyright (C) 2022 v0lt
 *  Based on the following code:
 *  DC-Bass Source filter - http://www.dsp-worx.de/index.php?n=15
 *  DC-Bass Source Filter C++ porting - https://github.com/frafv/DCBassSource
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 */

#include "StdAfx.h"
#include "Common.h"
#include "BassDecoder.h"
#include "BassSource.h"
#include <../Include/bass.h>
#include <../Include/bass_aac.h>
#include "BassAudioSource.h"

/*** Utilities ****************************************************************/

LPWSTR GetFilterDirectory(LPWSTR folder)
{
	WCHAR PathBuffer[MAX_PATH + 1];

	DWORD res = GetModuleFileNameW(HInstance, PathBuffer, (DWORD)std::size(PathBuffer));
	if (!res) {
		*folder = 0;
		return folder;
	}

	return GetFilePath(PathBuffer, folder);
}

bool IsMODFile(LPCWSTR fileName)
{
	static LPCWSTR mod_exts[] = { L".it", L".mo3", L".mod", L".mptm", L".mtm", L".s3m", L".umx", L".xm"};

	LPWSTR ext;
	WCHAR PathBuffer[MAX_PATH + 1];

	ext = GetFileExt(fileName, PathBuffer);

	for (const auto& mod_ext : mod_exts) {
		if (lstrcmpiW(mod_ext, ext) == 0) {
			return true;
		}
	}

	return false;
}

bool IsURLPath(LPCWSTR fileName)
{
	return wcsstr(fileName, L"http://") || wcsstr(fileName, L"ftp://");
}

/*** Callbacks ****************************************************************/

void CALLBACK OnMetaData(HSYNC handle, DWORD channel, DWORD data, void* user)
{
	BassDecoder* decoder = (BassDecoder*)user;

	if (decoder->m_shoutcastEvents) {
		WCHAR TextBuffer[1024];

		LPWSTR metaStr = (LPWSTR)FromUtf8ToWide(BASS_ChannelGetTags(channel, BASS_TAG_META), TextBuffer, (int)std::size(TextBuffer));
		LPWSTR resStr = L"";

		LPWSTR idx = wcsstr(metaStr, L"StreamTitle='");
		if (idx) {
			// Shoutcast Metadata
			resStr = idx + 13;
			if (*resStr) {
				resStr[wcslen(resStr) - 1] = 0;
			}
			//LPWSTR idx2 = wcsstr(resStr, L"';");
			//if (idx2) {
			//	*idx2 = 0;
			//} else
			//{
			idx = wcsstr(resStr, L"'");
			if (idx) {
				*idx = 0;
			}
			//}
		}
		else if ((idx = wcsstr(metaStr, L"TITLE=")) ||
				(idx = wcsstr(metaStr, L"Title=")) ||
				(idx = wcsstr(metaStr, L"title="))) {
			resStr = idx + 6;
		}

		decoder->m_shoutcastEvents->OnShoutcastMetaDataCallback(resStr);
	}
}

void CALLBACK OnShoutcastData(const void* buffer, DWORD length, void* user)
{
	BassDecoder* decoder = (BassDecoder*)user;
	if (buffer && decoder->m_shoutcastEvents) {
		decoder->m_shoutcastEvents->OnShoutcastBufferCallback(buffer, length);
	}
}

//
// BassDecoder
//

BassDecoder::BassDecoder(ShoutcastEvents* shoutcastEvents, int buffersizeMS, int prebufferMS)
	: m_shoutcastEvents(shoutcastEvents)
	, m_buffersizeMS(buffersizeMS)
	, m_prebufferMS(prebufferMS)
{
	LPWSTR path;
	WCHAR PathBuffer2[MAX_PATH + 1];

	path = wcscat(GetFilterDirectory(PathBuffer2), L"OptimFROG.dll");
	m_optimFROGDLL = LoadLibrary(path);

	LoadBASS();
	LoadPlugins();
}

BassDecoder::~BassDecoder()
{
	Close();

	if (m_optimFROGDLL) {
		FreeLibrary(m_optimFROGDLL);
	}
}

void BassDecoder::LoadBASS()
{
	BASS_Init(0, 44100, 0, GetDesktopWindow(), nullptr);

	if (m_prebufferMS == 0) {
		m_prebufferMS = 1;
	}

	BASS_SetConfigPtr(BASS_CONFIG_NET_AGENT, LABEL_BassAudioSource);
	BASS_SetConfig(BASS_CONFIG_NET_BUFFER, m_buffersizeMS);
	BASS_SetConfig(BASS_CONFIG_NET_PREBUF, m_buffersizeMS * 100 / m_prebufferMS);
}

void BassDecoder::UnloadBASS()
{
	try {
		if (InstanceCount == 0) {
			BASS_Free();
		}
	}
	catch (...) {
		// crashes mplayer2.exe ???
	}
}

void BassDecoder::LoadPlugins()
{
	LPWSTR path;
	WCHAR PathBuffer2[MAX_PATH + 1];

	path = GetFilterDirectory(PathBuffer2);
	LPWSTR plugin = path + wcslen(path);

	for (int i = 0; i < BASS_PLUGINS_COUNT; i++) {
		wcscpy(plugin, BASS_PLUGINS[i]);
		BASS_PluginLoad(LPCSTR(path), BASS_TFLAGS);
	}
}

bool BassDecoder::Load(LPCWSTR fileName)
{
	Close();

	m_isShoutcast = false;

	WCHAR TextBuffer[1024];

	int strPos = wcsncmp(fileName, L"icyx", 4);
	if (strPos == 0) {
		// replace ICYX
		TextBuffer[0] = 'h';
		TextBuffer[1] = 't';
		TextBuffer[2] = 't';
		TextBuffer[3] = 'p';
		wcscpy(TextBuffer + 4, fileName + 4);
		fileName = TextBuffer;
	}

	m_isMOD = IsMODFile(fileName);
	m_isURL = IsURLPath(fileName);

	if (m_isMOD) {
		if (!m_isURL) {
			m_stream = BASS_MusicLoad(false, (const void*)fileName, 0, 0, BASS_MUSIC_DECODE | BASS_MUSIC_RAMP | BASS_MUSIC_POSRESET | BASS_MUSIC_PRESCAN | BASS_TFLAGS, 0);
		}
	}
	else {
		if (m_isURL) {
			m_stream = BASS_StreamCreateURL(LPCSTR(fileName), 0, BASS_STREAM_DECODE | BASS_TFLAGS, OnShoutcastData, this);
			m_sync = BASS_ChannelSetSync(m_stream, BASS_SYNC_META, 0, OnMetaData, this);
			m_isShoutcast = GetDuration() == 0;
		}
		else {
			m_stream = BASS_StreamCreateFile(false, (const void*)fileName, 0, 0, BASS_STREAM_DECODE | BASS_TFLAGS);
		}
	}

	if (!m_stream) {
		return false;
	}

	if (!GetStreamInfos()) {
		Close();
		return false;
	}

	return true;
}

void BassDecoder::Close()
{
	if (!m_stream) {
		return;
	}

	if (m_sync) {
		BASS_ChannelRemoveSync(m_stream, m_sync);
	}

	if (m_isMOD) {
		BASS_MusicFree(m_stream);
	} else {
		BASS_StreamFree(m_stream);
	}

	m_channels = 0;
	m_sampleRate = 0;
	m_bytesPerSample = 0;
	m_float = false;
	m_mSecConv = 0;

	m_stream = 0;
}

int BassDecoder::GetData(void* buffer, int size)
{
	return BASS_ChannelGetData(m_stream, buffer, size);
}

bool BassDecoder::GetStreamInfos()
{
	BASS_CHANNELINFO info;

	if (!BASS_ChannelGetInfo(m_stream, &info)) {
		return false;
	}

	if (info.chans == 0 || info.freq == 0) {
		return false;
	}

	m_float = (info.flags & BASS_SAMPLE_FLOAT) != 0;
	m_sampleRate = info.freq;
	m_channels   = info.chans;
	m_type       = info.ctype;

	if (m_float) {
		m_bytesPerSample = 4;
	}
	else if (info.flags & BASS_SAMPLE_8BITS) {
		m_bytesPerSample = 1;
	}
	else {
		m_bytesPerSample = 2;
	}

	m_mSecConv = m_sampleRate * m_channels * m_bytesPerSample;

	if (m_mSecConv == 0) {
		return false;
	}

	GetHTTPInfos();

	return true;
}

void BassDecoder::GetNameTag(LPCSTR string)
{
	LPCWSTR tag;
	WCHAR TextBuffer[1024];

	if (!m_shoutcastEvents) {
		return;
	}

	LPCWSTR astring = FromAnsiToWide(string, TextBuffer, (int)std::size(TextBuffer));
	while (astring && *astring) {
		tag = astring;
		if (wcsncmp(L"icy-name:", tag, 9) == 0) {
			tag += 9;
			while (*tag && iswspace(*tag)) {
				tag++;
			}
			//if (m_shoutcastEvents)
			m_shoutcastEvents->OnShoutcastMetaDataCallback(tag);
		}

		astring += wcslen(astring) + 1;
	}
}

void BassDecoder::GetHTTPInfos()
{
	LPCSTR icyTags;
	LPCSTR httpHeaders;

	if (!m_isShoutcast) {
		return;
	}

	icyTags = BASS_ChannelGetTags(m_stream, BASS_TAG_ICY);
	if (icyTags) {
		GetNameTag(icyTags);
	}

	httpHeaders = BASS_ChannelGetTags(m_stream, BASS_TAG_HTTP);
	if (httpHeaders) {
		GetNameTag(httpHeaders);
	}

	LPCSTR metaTags = BASS_ChannelGetTags(m_stream, BASS_TAG_META);
	if (metaTags) {
		WCHAR TextBuffer[1024];

		LPWSTR metaStr = (LPWSTR)FromUtf8ToWide(metaTags, TextBuffer, (int)std::size(TextBuffer));
		LPWSTR resStr = L"";

		LPWSTR idx = wcsstr(metaStr, L"StreamTitle='");
		if (idx) {
			// Shoutcast Metadata
			resStr = idx + 13;
			if (*resStr) {
				resStr[wcslen(resStr) - 1] = 0;
			}
			//LPWSTR idx2 = wcsstr(resStr, L"';");
			//if (idx2) {
			//	*idx2 = 0;
			//} else
			//{
			idx = wcsstr(resStr, L"'");
			if (idx) {
				*idx = 0;
			}
			//}
		}
		else if ((idx = wcsstr(metaStr, L"TITLE=")) ||
			(idx = wcsstr(metaStr, L"Title=")) ||
			(idx = wcsstr(metaStr, L"title="))) {
			resStr = idx + 6;
		}

		m_shoutcastEvents->OnShoutcastMetaDataCallback(resStr);
	}
}

LONGLONG BassDecoder::GetDuration()
{
	if (m_mSecConv == 0) {
		return 0;
	}
	if (!m_stream) {
		return 0;
	}

	// bytes = samplerate * channel * bytes_per_second
	// msecs = (bytes * 1000) / (samplerate * channels * bytes_per_second)

	return BASS_ChannelGetLength(m_stream, BASS_POS_BYTE) * 1000 / m_mSecConv;
}

LONGLONG BassDecoder::GetPosition()
{
	if (m_mSecConv == 0) {
		return 0;
	}
	if (!m_stream) {
		return 0;
	}

	return BASS_ChannelGetPosition(m_stream, BASS_POS_BYTE) * 1000 / m_mSecConv;
}

void BassDecoder::SetPosition(LONGLONG positionMS)
{
	LONGLONG pos;

	if (m_mSecConv == 0) {
		return;
	}
	if (!m_stream) {
		return;
	}

	pos = LONGLONG(positionMS * m_mSecConv) / 1000L;

	BASS_ChannelSetPosition(m_stream, pos, BASS_POS_BYTE);
}

LPCWSTR BassDecoder::GetExtension()
{
	switch (m_type) {
	case BASS_CTYPE_STREAM_AAC:  return L"aac";
	case BASS_CTYPE_STREAM_MP4:  return L"mp4";
	case BASS_CTYPE_STREAM_MP3:  return L"mp3";
	case BASS_CTYPE_STREAM_OGG:  return L"ogg";
	default:                     return L"mp3";
	}
}
