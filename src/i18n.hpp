#pragma once

#include <obs-module.h>

#include <QString>

// The text for a key in OBS's language (data/locale/*.ini).
inline QString mt(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}
